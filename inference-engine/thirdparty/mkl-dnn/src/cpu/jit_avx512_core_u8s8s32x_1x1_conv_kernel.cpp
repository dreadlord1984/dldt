/*******************************************************************************
* Copyright 2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
#include <float.h>
#include "c_types_map.hpp"
#include "nstl.hpp"
#include "type_helpers.hpp"
#include "mkldnn_thread.hpp"
#include "utils.hpp"
#include "cpu_memory.hpp"

#include "jit_uni_1x1_conv_utils.hpp"
#include "jit_avx512_core_u8s8s32x_1x1_conv_kernel.hpp"

#define GET_OFF(field) offsetof(jit_1x1_conv_call_s, field)

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;

using namespace Xbyak;

bool jit_avx512_core_u8s8s32x_1x1_conv_kernel::maybe_relu(int position)
{
    using namespace primitive_kind;
    const auto &p = attr_.post_ops_;

    if (position == 0) {
        /* relu before sum */
        return false
            || jcp.with_eltwise
            || p.contain(eltwise, 0)
            || (jcp.dst_dt == data_type::u8 && !p.contain(sum, 0));
    } else if (position == 1) {
        /* relu after sum */
        const int sum_idx = p.contain(sum, 0)
            ? 0 : (p.contain(sum, 1) ? 1 : -1);
        if (sum_idx == -1)
            return false;

        return false
            || p.contain(eltwise, sum_idx + 1)
            || jcp.dst_dt == data_type::u8;
    }

    return false;
}

void jit_avx512_core_u8s8s32x_1x1_conv_kernel::bcast_loop(int load_loop_blk)
{
    mov(aux1_reg_bcast_data, reg_bcast_data);
    mov(aux_reg_bcast_data, reg_bcast_data);

    mov(aux_reg_output_data, reg_output_data);
    mov(aux_reg_acc_s32, reg_acc_s32);

    mov(bcast_loop_iter, EVEX_compress_addr(rsp, bcast_loop_work_offt));

    Label bcast_loop;
    Label bcast_loop_tail;

    cmp(bcast_loop_iter, jcp.ur);
    jl(bcast_loop_tail, T_NEAR);

    L(bcast_loop); {
        assert(jcp.bcast_block % jcp.ur == 0);
        int num_substeps = jcp.bcast_block / jcp.ur;
        assert(num_substeps > 0 && num_substeps < 10);
        for (int i = 0; i < num_substeps; i++) {
            reduce_loop(load_loop_blk, jcp.ur, i, false);
            if (i < num_substeps - 1) {
                add(aux1_reg_bcast_data, jcp.bcast_loop_bcast_substep);
                add(aux_reg_output_data, jcp.bcast_loop_output_substep);
                int ws_offset =
                    (jcp.bcast_loop_output_substep / jcp.typesize_out)
                        * jcp.typesize_acc;
                add(aux_reg_acc_s32, ws_offset);
            }
            else {
                add(aux1_reg_bcast_data, jcp.bcast_loop_bcast_step
                    - (num_substeps - 1) * jcp.bcast_loop_bcast_substep);
                int output_offset = jcp.bcast_loop_output_step
                    - (num_substeps - 1) * jcp.bcast_loop_output_substep;
                add(aux_reg_output_data, output_offset);
                int ws_offset = (output_offset / jcp.typesize_out)
                    * jcp.typesize_acc;
                add(aux_reg_acc_s32, ws_offset);
            }
        }
        sub(bcast_loop_iter, jcp.bcast_block);
        cmp(bcast_loop_iter, jcp.bcast_block);
        jge(bcast_loop, T_NEAR);
    }

    L(bcast_loop_tail);
    if (jcp.ur_tail) {
        Label bcast_loop_tail_out;
        cmp(bcast_loop_iter, 0);
        jz(bcast_loop_tail_out, T_NEAR);
        reduce_loop(load_loop_blk, jcp.ur_tail, 0, true);
        L(bcast_loop_tail_out);
    }
}

void jit_avx512_core_u8s8s32x_1x1_conv_kernel::reduce_loop(int load_loop_blk,
         int ur, int substep, bool wraparound)
{
    auto vreg_load = [=](int i_load) {
        return Zmm(ur * load_loop_blk + i_load);
    };

    auto vreg_accum = [=](int i_load, int i_ur) {
        return Zmm(i_ur * load_loop_blk + i_load);
    };

    auto xreg_accum = [=](int i_load, int i_ur) {
        return Xmm(i_ur * load_loop_blk + i_load);
    };

    auto bias_ptr = [=](int i_load) {
        return EVEX_compress_addr(reg_bias_data,
                                  jcp.typesize_bia * jcp.oc_block * i_load);
    };
    auto scale_ptr = [=](int i_load) {
        return EVEX_compress_addr(reg_ptr_scales,
                    jcp.is_oc_scale * (sizeof(float) * jcp.oc_block * i_load));
    };

    auto bcast_ptr = [=](int i_reduce, int i_ur, bool bcast) {
        assert(i_ur < jcp.ur);
        assert(i_reduce <= jcp.reduce_loop_unroll);
        assert(jcp.reduce_loop_unroll == jcp.reduce_block);

        int offt = (jcp.reduce_dim * i_ur + i_reduce);

        return EVEX_compress_addr(aux_reg_bcast_data, jcp.typesize_in * offt,
                                bcast);
    };

    auto load_ptr = [=](int i_reduce, int i_load) {
        int u0 = i_reduce % jcp.reduce_loop_unroll;
        int u1 = i_reduce / jcp.reduce_loop_unroll;

        int offt = (i_load * jcp.reduce_dim + u0) * jcp.load_block;

        return EVEX_compress_addr(aux_reg_load_data,
                                  u1 * jcp.reduce_loop_load_step
                                  + jcp.typesize_in * offt);
    };

    auto output_ptr = [=](int i_load, int i_ur) {
        return EVEX_compress_addr(aux_reg_output_data,
            jcp.typesize_out * (jcp.load_dim * i_ur + i_load * jcp.load_block));
    };

    auto acc_s32_ptr = [=](int i_load, int i_ur) {
        return EVEX_compress_addr(aux_reg_acc_s32,
            jcp.typesize_acc * (jcp.load_dim * i_ur + i_load * jcp.load_block));
    };

    auto init = [=]() {
        Label l_first_load, l_ret;

        test(reg_reduce_pos_flag, FLAG_REDUCE_FIRST);
        jnz(l_first_load, T_NEAR); // FISRT load: if not zero jump to <l_first_load>

        for (int i_load = 0; i_load < load_loop_blk; ++i_load)
            for (int i_ur = 0; i_ur < ur; ++i_ur) {
                auto r = vreg_accum(i_load, i_ur);
                vmovups(r, acc_s32_ptr(i_load, i_ur));
            }
        jmp(l_ret, T_NEAR);

        L(l_first_load);
        for (int i_load = 0; i_load < load_loop_blk; ++i_load)
            for (int i_ur = 0; i_ur < ur; ++i_ur) {
                auto r = vreg_accum(i_load, i_ur);
                vpxord(r, r, r);
            }
        L(l_ret);
    };

    auto store = [=]() {
        Label l_update_acc, l_ret;

        test(reg_reduce_pos_flag, FLAG_REDUCE_LAST);
        jz(l_update_acc, T_NEAR); // LAST channel: if zero jump to <l_update_acc>

        const auto &p = attr_.post_ops_;
        const int sum_idx = p.find(primitive_kind::sum);
        const float *p_sum_scale = (sum_idx != -1)
            ? &p.entry_[sum_idx].sum.scale
            : nullptr;

        if (jcp.with_bias) {
            mov(EVEX_compress_addr(rsp, aux_reg_acc_s32_offt), aux_reg_acc_s32);
            mov(reg_bias_data, EVEX_compress_addr(rsp, reg_bias_data_offt));
        }
        mov(EVEX_compress_addr(rsp, reg_bcast_data_off), reg_bcast_data);
        mov(reg_ptr_scales, EVEX_compress_addr(rsp, reg_ptr_sum_scale_off));
        if (p_sum_scale && *p_sum_scale != 1.f) {
            mov(EVEX_compress_addr(rsp, reg_load_data_off), reg_load_data);
            mov(reg_ptr_sum_scale, (size_t)p_sum_scale);
        }
        vpxord(zmm_zero, zmm_zero, zmm_zero);
        for (int i_load = 0; i_load < load_loop_blk; ++i_load) {
            auto zmm_bias = zmm_tmp;
            if (jcp.with_bias) {
                switch (jcp.bia_dt) {
                case data_type::f32:
                case data_type::s32: vmovups(zmm_bias,
                                        bias_ptr(i_load)); break;
                case data_type::s8: vpmovsxbd(zmm_bias,
                                        bias_ptr(i_load)); break;
                case data_type::u8: vpmovzxbd(zmm_bias,
                                        bias_ptr(i_load)); break;
                default: assert(!"unsupported bias data type");
                }
                if (jcp.bia_dt != data_type::f32)
                    vcvtdq2ps(zmm_bias, zmm_bias);
            }
            for (int i_ur = 0; i_ur < ur; ++i_ur) {
                auto r = vreg_accum(i_load, i_ur);
                auto x = xreg_accum(i_load, i_ur);
                vcvtdq2ps(r, r);
                if (jcp.with_bias)
                    vaddps(r, r, zmm_bias);
                vmulps(r, r, scale_ptr(i_load));
                if (maybe_relu(0))
                    vmaxps(r, zmm_zero, r);
                if (p_sum_scale) { // post_op: sum
                    auto zmm_prev_dst = zmm_bcast;
                    switch (jcp.dst_dt) {
                    case data_type::f32:
                    case data_type::s32: vmovups(zmm_prev_dst,
                                            output_ptr(i_load, i_ur)); break;
                    case data_type::s8: vpmovsxbd(zmm_prev_dst,
                                            output_ptr(i_load, i_ur)); break;
                    case data_type::u8: vpmovzxbd(zmm_prev_dst,
                                            output_ptr(i_load, i_ur)); break;
                    default: assert(!"unsupported dst data type");
                    }
                    if (jcp.dst_dt != data_type::f32)
                        vcvtdq2ps(zmm_prev_dst, zmm_prev_dst);
                    if (*p_sum_scale == 1.f)
                        vaddps(r, zmm_prev_dst);
                    else
                        vfmadd231ps(r, zmm_prev_dst, zword_b[reg_ptr_sum_scale]);
                }
                if (maybe_relu(1))
                    vmaxps(r, zmm_zero, r);
                if (jcp.dst_dt != data_type::f32) {
                    if (attr_.round_mode_ == round_mode::nearest) {
                        vcvtps2dq(r | T_rn_sae, r);
                    } else if (attr_.round_mode_ == round_mode::down) {
                        vcvtps2dq(r | T_rd_sae, r);
                    } else
                        assert(!"unimplemented");
                }
                switch (jcp.dst_dt) {
                case data_type::f32:
                case data_type::s32: vmovups(output_ptr(i_load, i_ur), r); break;
                case data_type::s8: vpmovsdb(x, r);
                                    vmovups(output_ptr(i_load, i_ur), x); break;
                case data_type::u8: vpmovusdb(x, r);
                                    vmovups(output_ptr(i_load, i_ur), x); break;
                default: assert(!"unknown dst_dt");
                }
            }
        }
        if (jcp.with_bias)
            mov(aux_reg_acc_s32, EVEX_compress_addr(rsp, aux_reg_acc_s32_offt));
        mov(reg_bcast_data, EVEX_compress_addr(rsp, reg_bcast_data_off));
        if (p_sum_scale && *p_sum_scale != 1.f)
            mov(reg_load_data, EVEX_compress_addr(rsp, reg_load_data_off));
        jmp(l_ret, T_NEAR);

        L(l_update_acc);

        mov(aux_reg_bcast_data, EVEX_compress_addr(rsp, aux_reg_acc_s32_offt));
        for (int i_load = 0; i_load < load_loop_blk; ++i_load)
            for (int i_ur = 0; i_ur < ur; ++i_ur) {
                auto r = vreg_accum(i_load, i_ur);
                vmovups(acc_s32_ptr(i_load, i_ur), r);

            }
        L(l_ret);
    };

    auto compute = [=](Zmm vreg_acc, Zmm vreg_wei, Zmm vreg_src) {
        if (jcp.ver == ver_vnni) {
            vpdpbusd(vreg_acc, vreg_src, vreg_wei);
        } else {
            vpmaddubsw(zmm_tmp, vreg_src, vreg_wei);
            vpmaddwd(zmm_tmp, zmm_tmp, zmm_one);
            vpaddd(vreg_acc, vreg_acc, zmm_tmp);
        }
    };

    auto fma_block = [=](bool last_block) {
        int reduce_step = 4;
        for (int i_reduce = 0; i_reduce < jcp.reduce_loop_unroll;
                i_reduce += reduce_step) {
            for (int i_load = 0; i_load < load_loop_blk; ++i_load)
                vmovups(vreg_load(i_load), load_ptr(i_reduce, i_load));
            for (int i_ur = 0; i_ur < ur; ++i_ur) {
                vpbroadcastd(zmm_bcast, bcast_ptr(i_reduce, i_ur, false));
                for (int i_load = 0; i_load < load_loop_blk; ++i_load) {
                    compute(vreg_accum(i_load, i_ur),
                                vreg_load(i_load), zmm_bcast);
                }
            }
        }
    };

    Label reduce_loop;
    Label reduce_loop_tail;

    mov(aux_reg_load_data, reg_load_data);

    mov(aux_reg_bcast_data, aux1_reg_bcast_data);
    init();

    mov(reduce_loop_iter, reg_reduce_loop_work);
    sub(reduce_loop_iter, jcp.reduce_loop_unroll);
    jle(reduce_loop_tail, T_NEAR);

    L(reduce_loop); {
        fma_block(false);
        add(aux_reg_bcast_data, jcp.reduce_loop_bcast_step);
        add(aux_reg_load_data, jcp.reduce_loop_load_step);
        sub(reduce_loop_iter, jcp.reduce_loop_unroll);
        jg(reduce_loop, T_NEAR);
    }

    L(reduce_loop_tail);
    fma_block(true);

    store();
}

void jit_avx512_core_u8s8s32x_1x1_conv_kernel::generate()
{
    preamble();

    xor_(reg_scratch, reg_scratch);
    Reg16 _t = reg_scratch.cvt16();
    mov(_t, 0x1);
    vpbroadcastw(zmm_one, _t);

    sub(rsp, stack_space_needed);
    if (jcp.with_bias) {
        mov(reg_bias_data, ptr[param1 + GET_OFF(bias_data)]);
        mov(EVEX_compress_addr(rsp, reg_bias_data_offt), reg_bias_data);
    }
    mov(reg_ptr_scales, ptr[param1 + GET_OFF(scales)]);
    mov(EVEX_compress_addr(rsp, reg_ptr_sum_scale_off), reg_ptr_scales);
    mov(reg_bcast_data, ptr[param1 + GET_OFF(bcast_data)]);
    mov(reg_load_data, ptr[param1 + GET_OFF(load_data)]);
    mov(reg_output_data, ptr[param1 + GET_OFF(output_data)]);

    mov(reg_acc_s32, ptr[param1 + GET_OFF(acc_s32)]);
    mov(reg_load_loop_work, ptr[param1 + GET_OFF(load_dim)]);
    mov(reg_bcast_loop_work, ptr[param1 + GET_OFF(bcast_dim)]);
    mov(EVEX_compress_addr(rsp, bcast_loop_work_offt), reg_bcast_loop_work);
    mov(reg_reduce_loop_work, ptr[param1 + GET_OFF(reduce_dim)]);
    mov(reg_reduce_pos_flag, ptr[param1 + GET_OFF(reduce_pos_flag)]);


    auto load_loop_body = [=](int load_loop_blk) {
        bcast_loop(load_loop_blk);
        add(reg_load_data, load_loop_blk * jcp.load_loop_load_step);
        if (jcp.with_bias) {
            mov(reg_bias_data, EVEX_compress_addr(rsp, reg_bias_data_offt));
            add(reg_bias_data,
                load_loop_blk * jcp.load_block * jcp.typesize_bia);
            mov(EVEX_compress_addr(rsp, reg_bias_data_offt), reg_bias_data);
        }
        mov(EVEX_compress_addr(rsp, reg_bcast_data_off), reg_bcast_data);
        mov(reg_ptr_scales, EVEX_compress_addr(rsp, reg_ptr_sum_scale_off));
        add(reg_ptr_scales,
            jcp.is_oc_scale * load_loop_blk * jcp.load_block * sizeof(float));
        mov(EVEX_compress_addr(rsp, reg_ptr_sum_scale_off), reg_ptr_scales);
        mov(reg_bcast_data, EVEX_compress_addr(rsp, reg_bcast_data_off));
        add(reg_output_data,
            load_loop_blk * jcp.load_block * jcp.typesize_out);
        add(reg_acc_s32,
            load_loop_blk * jcp.load_block * jcp.typesize_acc);
        sub(reg_load_loop_work, load_loop_blk * jcp.load_loop_iter_step);
    };

    const int simd_w = 16;

    Label load_loop_blk[7];

    static const int ur_cases_fma_expl_bcast[] = { 2, 5, 6, 9, 14, 32 };
    const int size_ur_cases_fma = sizeof(ur_cases_fma_expl_bcast);
    const int *ur_cases_fma = ur_cases_fma_expl_bcast;
    const int *ur_cases = ur_cases_fma;
    const int num_ur_cases = (size_ur_cases_fma) / sizeof(*ur_cases);

    for (int ur_idx = num_ur_cases - 1; ur_idx > 0; ur_idx--) {
        int label_idx = num_ur_cases - ur_idx - 1;
        if (jcp.ur <= ur_cases[ur_idx]) {
            cmp(reg_load_loop_work, simd_w * (label_idx + 1));
            jle(load_loop_blk[label_idx], T_NEAR);
        }
    }

    for (int ur_idx = 0; ur_idx < num_ur_cases; ur_idx++) {
        if (jcp.ur <= ur_cases[ur_idx]) {
            int label_idx = num_ur_cases - ur_idx - 1;
            L(load_loop_blk[label_idx]);
            {
                if (label_idx == 0) {
                    cmp(reg_load_loop_work, 0);
                    je(load_loop_blk[num_ur_cases], T_NEAR);
                }
                load_loop_body(label_idx + 1);
                if (label_idx - 1 > 0) {
                    cmp(reg_load_loop_work, 2 * label_idx * simd_w);
                    je(load_loop_blk[label_idx - 1], T_NEAR);
                }
                cmp(reg_load_loop_work, (label_idx + 1) * simd_w);
                jge(load_loop_blk[label_idx]);
            }
            for (int idx = label_idx - 1; idx > 0; --idx) {
                cmp(reg_load_loop_work, simd_w * (idx + 1));
                je(load_loop_blk[idx], T_NEAR);
            }
            if (ur_idx < num_ur_cases - 2) {
                cmp(reg_load_loop_work, simd_w);
                jle(load_loop_blk[0], T_NEAR);
            }
        }
    }
    L(load_loop_blk[num_ur_cases]);

    add(rsp, stack_space_needed);

    postamble();
}

bool jit_avx512_core_u8s8s32x_1x1_conv_kernel::post_ops_ok(
        jit_1x1_conv_conf_t &jcp, const primitive_attr_t &attr) {
    using namespace primitive_kind;
    const auto &p = attr.post_ops_;

    auto is_relu = [&](int idx) {
        return p.entry_[idx].kind == eltwise
            && p.entry_[idx].eltwise.scale == 1.
            && p.entry_[idx].eltwise.alg == alg_kind::eltwise_relu
            && p.entry_[idx].eltwise.alpha == 0.;
    };

   switch (p.len_) {
    case 0: return true;
    case 1: return true
                && implication(jcp.with_eltwise, p.contain(sum, 0))
                && implication(!jcp.with_eltwise, is_relu(0) || p.contain(sum, 0));
    case 2: return true
                && implication(jcp.with_eltwise, p.contain(sum, 0) && is_relu(1))
                && implication(!jcp.with_eltwise, false
                        || (p.contain(sum, 0) && is_relu(1))
                        || (p.contain(sum, 1) && is_relu(0)));
    case 3: return true
                && jcp.with_eltwise == false
                && (is_relu(0) && p.contain(sum, 1) && is_relu(2));
    default: return false;
    }

    return false;
}

status_t jit_avx512_core_u8s8s32x_1x1_conv_kernel::init_conf(
        jit_1x1_conv_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &src_d, const memory_desc_wrapper &weights_d,
        const memory_desc_wrapper &dst_d, const memory_desc_wrapper &bias_d,
        const primitive_attr_t &attr, bool with_relu, float relu_negative_slope,
        int nthreads, bool reduce_src)
{
    if (!mayiuse(avx512_core)) return status::unimplemented;

    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;
    if (src_d.data_type() != data_type::u8
        || weights_d.data_type() != data_type::s8
        || !one_of(dst_d.data_type(),
            data_type::f32, data_type::s32, data_type::s8, data_type::u8))
        return status::unimplemented;
    if (!one_of(weights_d.format(), gOIhw4i16o4i, OIhw4i16o4i))
        return status::unimplemented;

    jcp.ver = ver_avx512_core;
    if (mayiuse(avx512_core_vnni))
        jcp.ver = ver_vnni;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];
    jcp.oc = dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;
    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = dst_d.dims()[2];
    jcp.ow = dst_d.dims()[3];
    jcp.kh = weights_d.dims()[with_groups + 2];
    jcp.kw = weights_d.dims()[with_groups + 3];
    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];
    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];
    jcp.src_fmt = src_d.format();
    jcp.with_bias = cd.bias_desc.format != memory_format::undef;
    jcp.with_eltwise = with_relu;
    jcp.eltwise_alpha = relu_negative_slope;
    if (!implication(with_relu, relu_negative_slope == 0.))
        return status::unimplemented;

    jcp.os = jcp.oh * jcp.ow;
    jcp.is = jcp.ih * jcp.iw;
    jcp.tr_is = rnd_up(jcp.is, 4);

    if (!post_ops_ok(jcp, attr))
        return status::unimplemented;

    bool args_ok = true
        && jcp.ngroups == 1
        && src_d.format() == nhwc
        && one_of(cd.bias_desc.format, memory_format::undef, any, x)
        && dst_d.format() == nhwc;
    if (!args_ok) return status::unimplemented;

    const int simd_w = 16;

    args_ok = true
        && jcp.oc % simd_w == 0 && jcp.ic % simd_w == 0
        && jcp.t_pad == 0 && jcp.l_pad == 0
        && jcp.stride_w == 1 && jcp.stride_h == 1 // TODO: support some strides
        && jcp.kh == 1 && jcp.kw == 1;
    if (!args_ok) return status::unimplemented;

    jcp.bia_dt = jcp.with_bias ? cd.bias_desc.data_type : data_type::undef;
    jcp.dst_dt = cd.dst_desc.data_type;

    jcp.ic_block = jcp.oc_block = simd_w;

    jcp.typesize_in = types::data_type_size(src_d.data_type());
    jcp.typesize_out = types::data_type_size(dst_d.data_type());
    jcp.typesize_acc = sizeof(int32_t);
    jcp.typesize_bia = jcp.with_bias
        ? types::data_type_size(bias_d.data_type())
        : 0;

    const int SMALL_SPATIAL = 7 * 7;
    const int BIG_REDUCE_DIM = 1024;

    int load_blocking = 0;
    int load_blocking_max = 0;
    int bcast_blocking = 0;
    int bcast_blocking_max = 0;
    int reduce_blocking = 0;
    int reduce_blocking_max = 0;
    jcp.load_grp_count = 1;
    jcp.use_vmovntps = false;

    const int L2_size = get_cache_size(2, true) / sizeof(jcp.typesize_in);
    const int L2_capacity = (L2_size * 3) / 4;

    int size_treshold = 28;
    int max_regs = (jcp.ver == ver_vnni) ? 9 : 8;
    int min_regs = 6;
    jcp.expl_bcast = true;

    const int spatial = jcp.oh;
    jcp.ur = 1;
    for (int ur_w = max_regs; ur_w >= min_regs; ur_w--) {
        if ((spatial >= size_treshold && spatial % ur_w == 0)
                || (spatial < size_treshold && jcp.os % ur_w == 0)) {
            jcp.ur = ur_w;
            break;
        }
    }
    if (jcp.ur == 1) {
        jcp.ur = nstl::min(max_regs, jcp.os);
        int os_tail = jcp.os % max_regs;
        for (int i = max_regs; i >= min_regs; i--) {
            int i_tail = jcp.os % i;
            if (i_tail > os_tail || i_tail == 0) {
                jcp.ur = i;
                os_tail = i_tail;
                if (i_tail == 0)
                    break;
            }
        }
    }

    jcp.reduce_dim = jcp.ic;
    jcp.reduce_block = jcp.ic_block;

    jcp.load_dim = jcp.oc;
    jcp.load_block = jcp.oc_block;

    jcp.bcast_dim = jcp.is;

    jcp.bcast_block = jcp.ur;

    jcp.reduce_loop_unroll = jcp.reduce_block;
    jcp.reduce_loop_bcast_step
            = jcp.reduce_loop_unroll * jcp.typesize_in;

    jcp.reduce_loop_load_step
            = jcp.reduce_loop_unroll * jcp.load_block * jcp.typesize_in;

    jcp.bcast_loop_output_step = jcp.ur * jcp.load_dim * jcp.typesize_out;
    jcp.bcast_loop_output_substep = -1; // unused
    jcp.bcast_loop_bcast_step = jcp.ur * jcp.reduce_dim * jcp.typesize_in;
    jcp.bcast_loop_bcast_substep = -1; // unused

    jcp.load_loop_load_step
            = jcp.reduce_dim * jcp.load_block * jcp.typesize_in;

    jcp.load_loop_iter_step = jcp.load_block;

    jcp.loop_order = reduce_src ? loop_blr : loop_lbr;

    int nb_bcast = div_up(jcp.bcast_dim, jcp.bcast_block);
    int nb_reduce = div_up(jcp.reduce_dim, jcp.reduce_block);

    reduce_blocking = nb_reduce;
    if (jcp.bcast_dim <= SMALL_SPATIAL && jcp.reduce_dim >= BIG_REDUCE_DIM)
        reduce_blocking = 64;
    else if (jcp.bcast_dim > SMALL_SPATIAL && jcp.reduce_dim >= BIG_REDUCE_DIM)
        reduce_blocking = 16;
    reduce_blocking = best_divider(nb_reduce, 1, reduce_blocking, true);
    reduce_blocking *= jcp.reduce_block;

    bool cmp_reduce = reduce_blocking <= jcp.reduce_dim;
    if (cmp_reduce)
        jcp.loop_order = reduce_src ? loop_rbl : loop_rlb;
    load_blocking = jcp.load_dim;

    jcp.load_grp_count = div_up(nthreads, jcp.mb * jcp.ngroups * nb_bcast);
    jcp.load_grp_count = best_divider(
            nthreads, jcp.load_grp_count, 2 * jcp.load_grp_count, false);

    if (jcp.bcast_dim <= 64 && jcp.load_dim * jcp.reduce_dim >= L2_size) {
        jcp.load_grp_count = nstl::max(jcp.load_grp_count, 4);
    } else if (jcp.bcast_dim <= 49 && jcp.mb <= nthreads
            && jcp.load_dim > 512 && jcp.load_dim / jcp.reduce_dim >= 4) {
        jcp.load_grp_count = nstl::max(jcp.load_grp_count, 2); //
        load_blocking = jcp.load_block;
    }

    bcast_blocking = div_up(jcp.mb * jcp.ngroups * nb_bcast,
                             div_up(nthreads, jcp.load_grp_count)) * jcp.bcast_block;
    bcast_blocking = nstl::min(jcp.bcast_dim, bcast_blocking);
    bcast_blocking = rnd_up(bcast_blocking, jcp.bcast_block);

    int space_for_bcast
            = (L2_capacity - /* kernel_size - */
                2 * jcp.load_block * reduce_blocking
                    - jcp.ur * reduce_blocking - 3 * 1024);
    if (jcp.reduce_dim * jcp.bcast_dim > L2_capacity)
        space_for_bcast /= 2;

    int bcast_in_cache
            = nstl::max(jcp.bcast_block, space_for_bcast / reduce_blocking);
    bcast_blocking = nstl::min(
            bcast_blocking, rnd_dn(bcast_in_cache, jcp.bcast_block));

    load_blocking_max = load_blocking;
    bcast_blocking_max = bcast_blocking * 3 / 2;
    reduce_blocking_max = reduce_blocking;

    assert(load_blocking);
    assert(load_blocking_max);
    assert(bcast_blocking);
    assert(bcast_blocking_max);
    assert(reduce_blocking);
    assert(reduce_blocking_max);
    assert(load_blocking % jcp.load_block == 0);
    assert(reduce_blocking % jcp.reduce_block == 0);
    assert(load_blocking_max % jcp.load_block == 0);
    assert(reduce_blocking_max % jcp.reduce_block == 0);

    assert(jcp.reduce_loop_unroll % 4 == 0);
    assert(jcp.reduce_dim % jcp.reduce_loop_unroll == 0);

    assert(jcp.bcast_block % jcp.ur == 0);
    assert(jcp.reduce_dim % jcp.reduce_block == 0);

    jcp.ur_tail = jcp.bcast_dim % jcp.ur;

    jcp.nb_bcast_blocking = bcast_blocking / jcp.bcast_block;
    jcp.nb_bcast_blocking_max = bcast_blocking_max / jcp.bcast_block;
    jcp.nb_load_blocking = load_blocking / jcp.load_block;
    jcp.nb_load_blocking_max = load_blocking_max / jcp.load_block;
    jcp.nb_reduce_blocking = reduce_blocking / jcp.reduce_block;
    jcp.nb_reduce_blocking_max = reduce_blocking_max / jcp.reduce_block;

    jcp.nb_bcast = div_up(jcp.bcast_dim, jcp.bcast_block);
    jcp.nb_load = div_up(jcp.load_dim, jcp.load_block);
    jcp.nb_reduce = div_up(jcp.reduce_dim, jcp.reduce_block);

    const auto &oscales = attr.output_scales_;
    jcp.is_oc_scale = oscales.mask_ == 1 << 1;
    assert(utils::implication(!jcp.is_oc_scale, oscales.mask_ == 0));

    return status::success;
}

}
}
}

/*******************************************************************************
* Copyright 2019-2021 Intel Corporation
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

#ifndef GPU_OCL_REF_SUM_HPP
#define GPU_OCL_REF_SUM_HPP

#include "common/primitive.hpp"
#include "common/reorder_pd.hpp"
#include "common/stream.hpp"
#include "gpu/gpu_primitive.hpp"
#include "gpu/gpu_resource.hpp"
#include "gpu/gpu_sum_pd.hpp"
#include "gpu/ocl/ocl_utils.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

struct ref_sum_t : public gpu_primitive_t {
    using gpu_primitive_t::gpu_primitive_t;
    struct pd_t : public gpu_sum_pd_t {
        using gpu_sum_pd_t::gpu_sum_pd_t;
        pd_t(const pd_t &rhs) : gpu_sum_pd_t(rhs) { clone_reorder_pds(rhs); }

        ~pd_t() = default;

        DECLARE_SUM_PD_T("ref:any", ref_sum_t);

        status_t init(engine_t *engine) {
            bool ok = gpu_sum_pd_t::init(engine) == status::success;
            if (!ok) return status::unimplemented;

            if (has_zero_dim_memory()) return status::success;

            for (int i = 0; i < n_; ++i) {
                auto r_impls = engine->get_reorder_implementation_list(
                        src_md(i), dst_acc_md());
                for (auto r = r_impls; *r; ++r) {
                    primitive_attr_t r_attr;
                    r_attr.set_scratchpad_mode(scratchpad_mode::user);
                    r_attr.output_scales_.set(scales_[i]);
                    if (i != 0) r_attr.post_ops_.append_sum(1.0);

                    reorder_pd_t *r_pd;
                    if ((*r)(&r_pd, engine, &r_attr, engine, src_md(i), engine,
                                dst_acc_md())
                            == status::success) {
                        reorder_pds_.emplace_back(r_pd);
                        break;
                    }
                }
            }

            if (need_output_reorder()) {
                auto r_impls = engine->get_reorder_implementation_list(
                        dst_acc_md(), dst_md());
                for (auto r = r_impls; *r; ++r) {
                    primitive_attr_t r_attr;
                    r_attr.set_scratchpad_mode(scratchpad_mode::user);
                    reorder_pd_t *r_pd = nullptr;
                    if ((*r)(&r_pd, engine, &r_attr, engine, dst_acc_md(),
                                engine, dst_md())
                            == status::success) {
                        reorder_pds_.emplace_back(r_pd);
                        break;
                    }
                }
            }

            ok = reorder_pds_.size() == (size_t)n_ + need_output_reorder();
            if (!ok) return status::unimplemented;

            init_scratchpad();
            return status::success;
        }

        void clone_reorder_pds(const pd_t &rhs) {
            reorder_pds_.clear();
            for (size_t i = 0; i < rhs.reorder_pds_.size(); ++i)
                reorder_pds_.emplace_back(rhs.reorder_pds_[i]->clone());
        }

        std::vector<std::unique_ptr<primitive_desc_t>> reorder_pds_;

    private:
        void init_scratchpad() {
            using namespace memory_tracking::names;
            auto scratchpad = scratchpad_registry().registrar();
            if (need_output_reorder()) {
                const memory_desc_wrapper dst_acc_d(dst_acc_md());
                scratchpad.book(key_sum_reduction, dst_acc_d.size(), 1,
                        OCL_BUFFER_ALIGNMENT);
            }

            for (size_t i = 0; i < reorder_pds_.size(); i++) {
                scratchpad.book(key_nested_multiple + (int)i,
                        reorder_pds_[i]->scratchpad_registry());
            }
        }
    };

    status_t init(engine_t *engine) override {
        const size_t n = pd()->reorder_pds_.size();
        reorders_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            pd()->reorder_pds_[i]->create_primitive(reorders_[i], engine);
        }
        return status::success;
    }

    status_t execute(const exec_ctx_t &ctx) const override {
        using namespace memory_tracking::names;

        if (pd()->has_zero_dim_memory()) return status::success;

        const auto n = pd()->n_inputs();
        exec_args_t r_args;

        std::unique_ptr<memory_t> p_temp_dst_acc;
        if (pd()->need_output_reorder()) {
            auto scratchpad = ctx.get_scratchpad_grantor().get_memory_storage(
                    key_sum_reduction);
            CHECK(safe_ptr_assign(p_temp_dst_acc,
                    new memory_t(ctx.stream()->engine(), pd()->dst_acc_md(),
                            memory_flags_t::use_runtime_ptr,
                            scratchpad->data_handle())));
        }

        auto dst = ctx.args().at(DNNL_ARG_DST);
        memory_arg_t dst_acc = {p_temp_dst_acc.get(), false};

        for (int i = 0; i < n; ++i) {
            r_args[DNNL_ARG_SRC] = ctx.args().at(DNNL_ARG_MULTIPLE_SRC + i);
            r_args[DNNL_ARG_DST] = pd()->need_output_reorder() ? dst_acc : dst;
            exec_ctx_t r_ctx(ctx, std::move(r_args));

            nested_scratchpad_t ns(ctx, key_nested_multiple + i, reorders_[i]);
            r_ctx.set_scratchpad_grantor(ns.grantor());
            CHECK(reorders_[i]->execute(r_ctx));
#ifndef DNNL_SYCL_CUDA
            ctx.stream()->wait();
#endif
        }

        if (pd()->need_output_reorder()) {
            dst_acc = {p_temp_dst_acc.get(), true};
            r_args[DNNL_ARG_SRC] = dst_acc;
            r_args[DNNL_ARG_DST] = dst;
            exec_ctx_t r_ctx(ctx, std::move(r_args));

            nested_scratchpad_t ns(ctx, key_nested_multiple + n, reorders_[n]);
            r_ctx.set_scratchpad_grantor(ns.grantor());
            CHECK(reorders_[n]->execute(r_ctx));
        }

        return status::success;
    }

protected:
    primitive_list_t nested_primitives() const override {
        std::vector<const primitive_t *> _reorders;
        _reorders.reserve(reorders_.size());
        for (const auto &r : reorders_)
            _reorders.push_back(r.get());
        return _reorders;
    }

private:
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
    std::vector<std::shared_ptr<primitive_t>> reorders_;
};

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif

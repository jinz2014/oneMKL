/*******************************************************************************
* Copyright Codeplay Software Ltd
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions
* and limitations under the License.
*
*
* SPDX-License-Identifier: Apache-2.0
*******************************************************************************/

#if __has_include(<sycl/sycl.hpp>)
#include <sycl/sycl.hpp>
#else
#include <CL/sycl.hpp>
#endif

#include <array>
#include <optional>

#include <portfft/portfft.hpp>

#include "oneapi/mkl/exceptions.hpp"

#include "oneapi/mkl/dft/detail/commit_impl.hpp"
#include "oneapi/mkl/dft/detail/descriptor_impl.hpp"
#include "oneapi/mkl/dft/detail/portfft/onemkl_dft_portfft.hpp"
#include "oneapi/mkl/dft/types.hpp"

#include "portfft_helper.hpp"

// alias to avoid ambiguity
namespace pfft = portfft;

namespace oneapi::mkl::dft::portfft {
namespace detail {

template <dft::precision prec, dft::domain dom>
class portfft_commit final : public dft::detail::commit_impl<prec, dom> {
private:
    using scalar_type = typename dft::detail::commit_impl<prec, dom>::scalar_type;
    using fwd_type = typename dft::detail::commit_impl<prec, dom>::fwd_type;
    using bwd_type = typename dft::detail::commit_impl<prec, dom>::bwd_type;
    using descriptor_type = typename dft::detail::descriptor<prec, dom>;

    static constexpr pfft::domain domain =
        dom == dft::domain::REAL ? pfft::domain::REAL : pfft::domain::COMPLEX;
    // since only complex-to-complex transforms are supported, we expect both directions to be valid or neither.
    std::array<storage_type<descriptor_type>, 2> committed_descriptors = { std::nullopt,
                                                                           std::nullopt };

public:
    portfft_commit(sycl::queue& queue, const dft::detail::dft_values<prec, dom>&)
            : oneapi::mkl::dft::detail::commit_impl<prec, dom>(queue, backend::portfft) {
        if constexpr (prec == dft::detail::precision::DOUBLE) {
            if (!queue.get_device().has(sycl::aspect::fp64)) {
                throw mkl::exception("DFT", "commit", "Device does not support double precision.");
            }
        }
    }

    void commit(const dft::detail::dft_values<prec, dom>& config_values) override {
        // not available in portFFT:
        if (config_values.workspace != config_value::ALLOW) {
            throw mkl::unimplemented("dft/backends/portfft", __FUNCTION__,
                                     "portFFT only supports ALLOW for the WORKSPACE parameter");
        }
        if (config_values.ordering != config_value::ORDERED) {
            throw mkl::unimplemented("dft/backends/portfft", __FUNCTION__,
                                     "portFFT only supports ORDERED for the ORDERING parameter");
        }
        if (config_values.transpose) {
            throw mkl::unimplemented("dft/backends/portfft", __FUNCTION__,
                                     "portFFT does not supported transposed output");
        }

        // forward descriptor
        pfft::descriptor<scalar_type, domain> fwd_desc(
            { config_values.dimensions.cbegin(), config_values.dimensions.cend() });
        fwd_desc.forward_scale = config_values.fwd_scale;
        fwd_desc.backward_scale = config_values.bwd_scale;
        fwd_desc.number_of_transforms =
            static_cast<std::size_t>(config_values.number_of_transforms);
        fwd_desc.complex_storage = config_values.complex_storage == config_value::COMPLEX_COMPLEX
                                       ? pfft::complex_storage::INTERLEAVED_COMPLEX
                                       : pfft::complex_storage::SPLIT_COMPLEX;
        fwd_desc.placement = config_values.placement == config_value::INPLACE
                                 ? pfft::placement::IN_PLACE
                                 : pfft::placement::OUT_OF_PLACE;
        fwd_desc.forward_offset = static_cast<std::size_t>(config_values.input_strides[0]);
        fwd_desc.backward_offset = static_cast<std::size_t>(config_values.output_strides[0]);
        fwd_desc.forward_strides = { config_values.input_strides.cbegin() + 1,
                                     config_values.input_strides.cend() };
        fwd_desc.backward_strides = { config_values.output_strides.cbegin() + 1,
                                      config_values.output_strides.cend() };
        fwd_desc.forward_distance = static_cast<std::size_t>(config_values.fwd_dist);
        fwd_desc.backward_distance = static_cast<std::size_t>(config_values.bwd_dist);

        // backward descriptor
        pfft::descriptor<scalar_type, domain> bwd_desc(
            { config_values.dimensions.cbegin(), config_values.dimensions.cend() });
        bwd_desc.forward_scale = config_values.fwd_scale;
        bwd_desc.backward_scale = config_values.bwd_scale;
        bwd_desc.number_of_transforms =
            static_cast<std::size_t>(config_values.number_of_transforms);
        bwd_desc.complex_storage = config_values.complex_storage == config_value::COMPLEX_COMPLEX
                                       ? pfft::complex_storage::INTERLEAVED_COMPLEX
                                       : pfft::complex_storage::SPLIT_COMPLEX;
        bwd_desc.placement = config_values.placement == config_value::INPLACE
                                 ? pfft::placement::IN_PLACE
                                 : pfft::placement::OUT_OF_PLACE;
        bwd_desc.forward_offset = static_cast<std::size_t>(config_values.output_strides[0]);
        bwd_desc.backward_offset = static_cast<std::size_t>(config_values.input_strides[0]);
        bwd_desc.forward_strides = { config_values.output_strides.cbegin() + 1,
                                     config_values.output_strides.cend() };
        bwd_desc.backward_strides = { config_values.input_strides.cbegin() + 1,
                                      config_values.input_strides.cend() };
        bwd_desc.forward_distance = static_cast<std::size_t>(config_values.fwd_dist);
        bwd_desc.backward_distance = static_cast<std::size_t>(config_values.bwd_dist);

        try {
            auto q = this->get_queue();
            committed_descriptors[0] = fwd_desc.commit(q);
            committed_descriptors[1] = bwd_desc.commit(q);
        }
        catch (const pfft::unsupported_configuration& e) {
            throw oneapi::mkl::unimplemented("dft/backends/portfft", __FUNCTION__, e.what());
        }
    }

    ~portfft_commit() override = default;

    void* get_handle() noexcept override {
        return committed_descriptors.data();
    }

    // All the compute functions are implementated here so they are in the same translation unit as the commit function.
    // If the use of the kernel bundle is in a seperate translation unit from the one it was translated in, the runtime can fail to find it.

    // forward inplace COMPLEX_COMPLEX
    void forward_ip_cc(descriptor_type& desc, sycl::buffer<fwd_type, 1>& inout) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            detail::get_descriptors(desc)[0]->compute_forward(inout);
        }
    }
    sycl::event forward_ip_cc(descriptor_type& desc, fwd_type* inout,
                              const std::vector<sycl::event>& dependencies) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            return detail::get_descriptors(desc)[0]->compute_forward(inout, dependencies);
        }
        else {
            return {};
        }
    }

    // forward inplace REAL_REAL
    void forward_ip_rr(descriptor_type&, sycl::buffer<scalar_type, 1>&,
                       sycl::buffer<scalar_type, 1>&) override {
        throw oneapi::mkl::unimplemented("DFT", "compute_forward(desc, inout_re, inout_im)",
                                         "portFFT does not support real-real complex storage.");
    }
    sycl::event forward_ip_rr(descriptor_type&, scalar_type*, scalar_type*,
                              const std::vector<sycl::event>&) override {
        throw oneapi::mkl::unimplemented("DFT",
                                         "compute_forward(desc, inout_re, inout_im, dependencies)",
                                         "portFFT does not support real-real complex storage.");
    }

    // forward out-of-place COMPLEX_COMPLEX
    void forward_op_cc(descriptor_type& desc, sycl::buffer<fwd_type, 1>& in,
                       sycl::buffer<bwd_type, 1>& out) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            detail::get_descriptors(desc)[0]->compute_forward(in, out);
        }
    }
    sycl::event forward_op_cc(descriptor_type& desc, fwd_type* in, bwd_type* out,
                              const std::vector<sycl::event>& dependencies) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            return detail::get_descriptors(desc)[0]->compute_forward(in, out, dependencies);
        }
        else {
            return {};
        }
    }

    // forward out-of-place REAL_REAL
    void forward_op_rr(descriptor_type&, sycl::buffer<scalar_type, 1>&,
                       sycl::buffer<scalar_type, 1>&, sycl::buffer<scalar_type, 1>&,
                       sycl::buffer<scalar_type, 1>&) override {
        throw oneapi::mkl::unimplemented("DFT",
                                         "compute_forward(desc, in_re, in_im, out_re, out_im)",
                                         "portFFT does not support real-real complex storage.");
    }
    sycl::event forward_op_rr(descriptor_type&, scalar_type*, scalar_type*, scalar_type*,
                              scalar_type*, const std::vector<sycl::event>&) override {
        throw oneapi::mkl::unimplemented(
            "DFT", "compute_forward(desc, in_re, in_im, out_re, out_im, dependencies)",
            "portFFT does not support real-real complex storage.");
    }

    // backward inplace COMPLEX_COMPLEX
    void backward_ip_cc(descriptor_type& desc, sycl::buffer<fwd_type, 1>& inout) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            detail::get_descriptors(desc)[1]->compute_backward(inout);
        }
    }
    sycl::event backward_ip_cc(descriptor_type& desc, fwd_type* inout,
                               const std::vector<sycl::event>& dependencies) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            return detail::get_descriptors(desc)[1]->compute_backward(inout, dependencies);
        }
        else {
            return {};
        }
    }

    // backward inplace REAL_REAL
    void backward_ip_rr(descriptor_type&, sycl::buffer<scalar_type, 1>&,
                        sycl::buffer<scalar_type, 1>&) override {
        throw oneapi::mkl::unimplemented("DFT", "compute_backward(desc, inout_re, inout_im)",
                                         "portFFT does not support real-real complex storage.");
    }
    sycl::event backward_ip_rr(descriptor_type&, scalar_type*, scalar_type*,
                               const std::vector<sycl::event>&) override {
        throw oneapi::mkl::unimplemented("DFT",
                                         "compute_backward(desc, inout_re, inout_im, dependencies)",
                                         "portFFT does not support real-real complex storage.");
    }

    // backward out-of-place COMPLEX_COMPLEX
    void backward_op_cc(descriptor_type& desc, sycl::buffer<bwd_type, 1>& in,
                        sycl::buffer<fwd_type, 1>& out) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            detail::get_descriptors(desc)[1]->compute_backward(in, out);
        }
    }
    sycl::event backward_op_cc(descriptor_type& desc, bwd_type* in, fwd_type* out,
                               const std::vector<sycl::event>& dependencies) override {
        constexpr auto pfft_domain = detail::to_pfft_domain<descriptor_type>::type::value;

        if constexpr (pfft_domain == pfft::domain::COMPLEX) {
            return detail::get_descriptors(desc)[1]->compute_backward(in, out, dependencies);
        }
        else {
            return {};
        }
    }

    // backward out-of-place REAL_REAL
    void backward_op_rr(descriptor_type&, sycl::buffer<scalar_type, 1>&,
                        sycl::buffer<scalar_type, 1>&, sycl::buffer<scalar_type, 1>&,
                        sycl::buffer<scalar_type, 1>&) override {
        throw oneapi::mkl::unimplemented("DFT",
                                         "compute_backward(desc, in_re, in_im, out_re, out_im)",
                                         "portFFT does not support real-real complex storage.");
    }
    sycl::event backward_op_rr(descriptor_type&, scalar_type*, scalar_type*, scalar_type*,
                               scalar_type*, const std::vector<sycl::event>&) override {
        throw oneapi::mkl::unimplemented(
            "DFT", "compute_backward(desc, in_re, in_im, out_re, out_im, deps)",
            "portFFT does not support real-real complex storage.");
    }
};
} // namespace detail

template <dft::precision prec, dft::domain dom>
dft::detail::commit_impl<prec, dom>* create_commit(const dft::detail::descriptor<prec, dom>& desc,
                                                   sycl::queue& sycl_queue) {
    return new detail::portfft_commit<prec, dom>(sycl_queue, desc.get_values());
}

template dft::detail::commit_impl<dft::detail::precision::SINGLE, dft::detail::domain::REAL>*
create_commit(
    const dft::detail::descriptor<dft::detail::precision::SINGLE, dft::detail::domain::REAL>&,
    sycl::queue&);
template dft::detail::commit_impl<dft::detail::precision::SINGLE, dft::detail::domain::COMPLEX>*
create_commit(
    const dft::detail::descriptor<dft::detail::precision::SINGLE, dft::detail::domain::COMPLEX>&,
    sycl::queue&);
template dft::detail::commit_impl<dft::detail::precision::DOUBLE, dft::detail::domain::REAL>*
create_commit(
    const dft::detail::descriptor<dft::detail::precision::DOUBLE, dft::detail::domain::REAL>&,
    sycl::queue&);
template dft::detail::commit_impl<dft::detail::precision::DOUBLE, dft::detail::domain::COMPLEX>*
create_commit(
    const dft::detail::descriptor<dft::detail::precision::DOUBLE, dft::detail::domain::COMPLEX>&,
    sycl::queue&);

} // namespace oneapi::mkl::dft::portfft

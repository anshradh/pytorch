// Basic functions on sparse tensors
#define TORCH_ASSERT_ONLY_METHOD_OPERATORS

#include <ATen/core/Tensor.h>
#include <ATen/Dispatch.h>
#include <ATen/InitialTensorOptions.h>
#include <ATen/Layout.h>
#include <ATen/Parallel.h>
#include <ATen/SparseCsrTensorImpl.h>
#include <ATen/SparseCsrTensorUtils.h>
#include <ATen/SparseTensorImpl.h>
#include <ATen/native/LinearAlgebraUtils.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/_convert_indices_from_csr_to_coo.h>
#include <ATen/ops/_nnz_native.h>
#include <ATen/ops/_sparse_compressed_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_csr_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_csc_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_bsr_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_bsc_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_coo_tensor_unsafe_native.h>
#include <ATen/ops/_validate_sparse_compressed_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_csr_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_csc_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_bsr_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_bsc_tensor_args_native.h>
#include <ATen/ops/aminmax.h>
#include <ATen/ops/ccol_indices_native.h>
#include <ATen/ops/clone_native.h>
#include <ATen/ops/col_indices_native.h>
#include <ATen/ops/copy_native.h>
#include <ATen/ops/crow_indices_native.h>
#include <ATen/ops/dense_dim_native.h>
#include <ATen/ops/empty.h>
#include <ATen/ops/empty_like_native.h>
#include <ATen/ops/empty_native.h>
#include <ATen/ops/resize_as_sparse_native.h>
#include <ATen/ops/resize_native.h>
#include <ATen/ops/row_indices_native.h>
#include <ATen/ops/select_native.h>
#include <ATen/ops/sparse_compressed_tensor_native.h>
#include <ATen/ops/sparse_csr_tensor_native.h>
#include <ATen/ops/sparse_csc_tensor_native.h>
#include <ATen/ops/sparse_bsr_tensor_native.h>
#include <ATen/ops/sparse_bsc_tensor_native.h>
#include <ATen/ops/sparse_dim_native.h>
#include <ATen/ops/values_native.h>
#endif

namespace at {
namespace native {

using namespace at::sparse_csr;

namespace {


} // end anonymous namespace

/*
  Validate the arguments to sparse compressed (CSR, CSC, BSR, and BSC)
  tensor factory functions.

  The CSR and BSR invariants for PyTorch are outlined in

    https://pearu.github.io/csr_tensor_invariants.html
    https://pearu.github.io/bsr_tensor_invariants.html

  that in what follows are generalized for all sparse compressed
  formats with support to batched and dense dimensions.
*/

void _validate_sparse_compressed_tensor_args_worker(const Tensor& compressed_indices, const Tensor& plain_indices, const Tensor& values, const IntArrayRef size, const Layout& layout) {
  // Layout must be Sparse Compressed, 2.4
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout, "validate_sparse_compressed_tensor_args", [&]{});

  const std::string layout_name = layoutToString(layout, /*upper=*/ true);
  const std::string compressed_indices_name = compressedIndicesName(layout);
  const std::string plain_indices_name = plainIndicesName(layout);
  const std::string compressed_dim_name = compressedDimName(layout);
  const std::string plain_dim_name = plainDimName(layout);

  // Layout Invariants
  // 2.1, 3.5
  TORCH_CHECK(
              plain_indices.layout() == kStrided && plain_indices.is_contiguous(),
              "expected ", plain_indices_name, " to be a strided and contiguous tensor");

  // 2.2, 3.6
  TORCH_CHECK(
              compressed_indices.layout() == kStrided && compressed_indices.is_contiguous(),
              "expected ", compressed_indices_name ," to be a strided and contiguous tensor");

  // 2.3, partially 3.7
  // TODO: allow values be contiguous along both block dimensions when the format is BSR or BSC
  TORCH_CHECK(
      values.layout() == kStrided && values.is_contiguous(),
      "expected values to be a strided and contiguous tensor");

  const int base_ndim = 2;  // corresponds to compressed and plain indices
  const int batch_ndim = compressed_indices.dim() - 1;
  const int block_ndim = AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(
                           layout, "validate_sparse_compressed_tensor_args",
                           [&] { return 0; }, [&] { return 2; });
  const int dense_ndim = values.dim() - batch_ndim - block_ndim - 1;
  // Shape and Strides invariants

  // 3.2
  TORCH_CHECK(
              batch_ndim >= 0,
              compressed_indices_name, " must have dimensionality >= 1 but got ", compressed_indices.dim());

  // 3.3
  TORCH_CHECK(
              compressed_indices.dim() == plain_indices.dim(),
              compressed_indices_name, " and ", plain_indices_name, " dimensionalities must be equal but got ",
              compressed_indices.dim(), " and ", plain_indices.dim(), ", respectively");

  // 3.4
  TORCH_CHECK(
              dense_ndim >= 0,
              "values must have dimensionality > sum of batch and block dimensionalities (=",
              batch_ndim, " + ", block_ndim, ") but got ", values.dim());
  // 3.1
  TORCH_CHECK(
              static_cast<int>(size.size()) == batch_ndim + base_ndim + dense_ndim,
              "tensor dimensionality must be sum of batch, base, and dense dimensionalites (=",
              batch_ndim, " + ", base_ndim, " + ", dense_ndim, ") but got ", size.size());

  // For CSR/CSC formats, we define blocksize=(1, 1) so that checking
  // the sparse compressed tensor invariants can be unified with the
  // BSR/BSC invariants.
  // 3.10
  DimVector blocksize{
                      (block_ndim == 2 ? std::max<int64_t>(1, values.size(batch_ndim + 1)) : 1),
                      (block_ndim == 2 ? std::max<int64_t>(1, values.size(batch_ndim + 2)) : 1),
  };
  TORCH_INTERNAL_ASSERT(blocksize.size() == 2 && blocksize[0] > 0 && blocksize[1] > 0);

  // All batch sizes must be the same and consistent with tensor batchsize, 3.1, 3.8, 3.9, 3.10
  DimVector batchsize = DimVector(size.slice(0, batch_ndim));
  DimVector compressed_indices_batchsize = DimVector(compressed_indices.sizes().slice(0, batch_ndim));
  DimVector plain_indices_batchsize = DimVector(plain_indices.sizes().slice(0, batch_ndim));
  DimVector values_batchsize = DimVector(values.sizes().slice(0, batch_ndim));
  const int values_nnz = (values.numel() ? values.size(batch_ndim) : 0);
  DimVector values_blocksize = DimVector(values.sizes().slice(batch_ndim + 1, block_ndim));
  DimVector values_densesize = DimVector(values.sizes().slice(batch_ndim + 1 + block_ndim, dense_ndim));
  TORCH_CHECK(
      batchsize == compressed_indices_batchsize && batchsize == plain_indices_batchsize && batchsize == values_batchsize,
      "all batch dimensions of ", compressed_indices_name," (=", compressed_indices_batchsize, "), ", plain_indices_name," (=",
      plain_indices_batchsize, "), and values (=", values_batchsize, ") must be equal to tensor batch dimensions (=",
      batchsize, ")");

  // A tensor constitutes of full blocks, 3.1
  for (int i=0; i<block_ndim; i++) {
      TORCH_CHECK(size[batch_ndim + i] % blocksize[i] == 0,
                  "tensor shape[", batch_ndim + i, "] (=", size[batch_ndim + i],
                  ") must be divisible with blocksize[", i, "] (=", blocksize[i],
                  ") as defined by values shape");
  }
  const int nrows = size[batch_ndim] / blocksize[0];
  const int ncols = size[batch_ndim + 1] / blocksize[1];
  int ncompressed_dims, nplain_dims;
  std::tie(ncompressed_dims, nplain_dims) = AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(layout, "validate_sparse_compressed_tensor_args",
                                                                                      [&] { return std::make_tuple(nrows, ncols); },
                                                                                      [&] { return std::make_tuple(ncols, nrows); });
  // 3.8
  TORCH_CHECK(
              compressed_indices.size(-1) == ncompressed_dims + 1,
              compressed_indices_name, ".shape[-1] must be equal to the number of ",
              compressed_dim_name, "s + 1 (=", ncompressed_dims + 1, "), but got ", compressed_indices.size(-1));
  // 3.9, 3.10
  TORCH_CHECK(
              plain_indices.size(-1) == values_nnz,
              plain_indices_name, ".shape[-1] must be equal to nnz (=", values_nnz,
              ") as defined by values.shape[", batch_ndim, "], but got ", plain_indices.size(-1));
  // Type Invariants
  auto compressed_indices_type = compressed_indices.scalar_type();
  auto plain_indices_type = plain_indices.scalar_type();
  // 1.1, 1.2, 1.3
  TORCH_CHECK(
      compressed_indices_type == plain_indices_type,
      compressed_indices_name, " and ", plain_indices_name, " must have the same dtype, bot got ",
      compressed_indices_type, " and ", plain_indices_type, ", respectively");
  TORCH_CHECK(
      compressed_indices_type == kInt || compressed_indices_type == kLong,
      compressed_indices_name, " and ", plain_indices_name, " dtype must be Int or Long, but got ",
      compressed_indices_type);

  // Indices invariants
  AT_DISPATCH_INDEX_TYPES(compressed_indices_type, "validate_sparse_compressed_tensor_args",
    [&] {
      if (plain_indices.numel() > 0) {
        Tensor compressed_indices_cpu = compressed_indices.to(kCPU);
        Tensor plain_indices_cpu = plain_indices.to(kCPU);
        int64_t batch_compressed_stride = compressed_indices_cpu.dim() >= 2 ? compressed_indices_cpu.stride(-2) : 0;
        int64_t batch_plain_stride = plain_indices_cpu.dim() >= 2 ? plain_indices_cpu.stride(-2) : 0;
        auto compressed_indices_data_ptr = compressed_indices_cpu.data_ptr<index_t>();
        auto plain_indices_data_ptr = plain_indices_cpu.data_ptr<index_t>();
        for (const auto batch_id : c10::irange(batchCount(compressed_indices_cpu))) {
          int64_t start = compressed_indices_data_ptr[batch_id*batch_compressed_stride];
          const std::string at_batch_id = (batch_ndim > 0 ? " at batch id " + std::to_string(batch_id) : "");
          const std::string batch_indices = (batch_ndim > 0 ? "..., " : "");
          // 5.1
          TORCH_CHECK(start == 0, compressed_indices_name, "[", batch_indices, "0] (=", start, ") == 0 is unsatisfied", at_batch_id);
          for (int i = 1; i <= ncompressed_dims; i++) {
            int64_t end = compressed_indices_data_ptr[batch_id*batch_compressed_stride + i];
            // 5.2
            TORCH_CHECK(end <= values_nnz,
                        compressed_indices_name, "[", batch_indices, i, "] (=", end, ") ",
                        "<= nnz (=", values_nnz, ") is unsatisfied", at_batch_id);
            // 5.3
            TORCH_CHECK(start <= end, compressed_indices_name, " must be ordered sequence but ",
                        compressed_indices_name, "[", batch_indices, i - 1, "] (=", start, ") <= ",
                        compressed_indices_name, "[", batch_indices, i, "] (=", end, ") is unsatisfied", at_batch_id);
            TORCH_CHECK(end - start <= nplain_dims,
                        compressed_indices_name, "[", batch_indices, i, "] (=", end, ") - ",
                        compressed_indices_name, "[", batch_indices, i - 1, "] (=", start, ") <= number of ",
                        plain_dim_name, "s (=", nplain_dims, ") is unsatisfied", at_batch_id);
            int64_t last_plain_index = -1;
            for (int n = start; n < end; n++) {
              int64_t plain_index = plain_indices_data_ptr[batch_id*batch_plain_stride + n];
              // 5.4, 5.5
              TORCH_CHECK(0 <= plain_index && plain_index < nplain_dims,
                          plain_indices_name, "[", batch_indices, n, "] (=", plain_index, ") is out of range (0, ", nplain_dims, ")", at_batch_id);
              // 5.6
              TORCH_CHECK(plain_index > last_plain_index, plain_indices_name, " must be ordered sequence of distinct integers but ",
                          plain_indices_name, "[", batch_indices, n - 1, "] (=", last_plain_index, ") < ",
                          plain_indices_name, "[", batch_indices, n, "] (=", plain_index, ") is unsatisfied", at_batch_id);
              last_plain_index = plain_index;
            }
            start = end;
          }
        }
      }
    });

  // Device Invariants
  // 4.1
  TORCH_CHECK(
      values.device().type() == kCPU || values.device().type() == kCUDA,
      "device type of values (",
      values.device().type(),
      ") must be CPU or CUDA");
  // 4.2, 4.3, 4.4
  TORCH_CHECK(
      compressed_indices.get_device() == values.get_device(),
      "device of ", compressed_indices_name, " (=",
      compressed_indices.device(),
      ") must match device of values (=",
      values.device(),
      ")");
  TORCH_CHECK(
      compressed_indices.get_device() == plain_indices.get_device(),
      "device of ", compressed_indices_name, " (=",
      compressed_indices.device(),
      ") must match device of ", plain_indices_name," (=",
      plain_indices.device(),
      ")");
}

void _validate_sparse_compressed_tensor_args(const Tensor& compressed_indices, const Tensor& plain_indices, const Tensor& values, IntArrayRef size, Layout layout) {
  _validate_sparse_compressed_tensor_args_worker(compressed_indices, plain_indices, values, size, layout);
}

void _validate_sparse_csr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(crow_indices, col_indices, values, size, kSparseCsr);
}

void _validate_sparse_csc_tensor_args(const Tensor& ccol_indices, const Tensor& row_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(ccol_indices, row_indices, values, size, kSparseCsc);
}

void _validate_sparse_bsr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(crow_indices, col_indices, values, size, kSparseBsr);
}

void _validate_sparse_bsc_tensor_args(const Tensor& ccol_indices, const Tensor& row_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(ccol_indices, row_indices, values, size, kSparseBsc);
}

// Construction of CSR, CSC, BSR, and BSC tensors.

// Note: The usage of "Csr" in names like SparseCsrTensor,
// SparseCsrCPU, SparseCsrCUDA, and SparseCsrTensorImpl exists because
// of historical reasons (that ought to be removed in future) and does
// not mean that the corresponding functionality would be CSR layout
// only specific.
SparseCsrTensor new_compressed_tensor(const TensorOptions& options) {
  // TODO: remove this comment after enabling autograd support for CSR tensor
  // constructor.
  // TORCH_INTERNAL_ASSERT(impl::variable_excluded_from_dispatch());
  Layout layout = AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(options.layout(), "new_compressed_tensor", [&] { return the_layout; });
  DispatchKey dispatch_key;

  TORCH_CHECK_NOT_IMPLEMENTED(
    options.device().type() == kCPU || options.device().type() == kCUDA,
     "Could not run 'new_compressed_tensor' from the '", options.device(), "' device.)");

  if (options.device().is_cuda()) {
    dispatch_key = DispatchKey::SparseCsrCUDA;
  } else {
    dispatch_key = DispatchKey::SparseCsrCPU;
  }

  return detail::make_tensor<SparseCsrTensorImpl>(
      DispatchKeySet(dispatch_key), layout, options.dtype());
}


Tensor _sparse_compressed_tensor_unsafe(const Tensor& compressed_indices,
                                        const Tensor& plain_indices,
                                        const Tensor& values,
                                        IntArrayRef size,
                                        c10::optional<ScalarType> dtype,
                                        c10::optional<Layout> layout,
                                        c10::optional<Device> device,
                                        c10::optional<bool> pin_memory) {
  if (!layout) {
    AT_ERROR("sparse_compressed_tensor_unsafe expected sparse compressed tensor layout but got none");
  }
  Layout layout_ = layout.value();
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout_, "sparse_compressed_tensor_unsafe", [&]{});
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);
  SparseCsrTensor self = new_compressed_tensor(options);
  get_sparse_csr_impl(self)->set_member_tensors(compressed_indices, plain_indices, values, size);
  return self;
}

template <Layout required_layout>
Tensor _sparse_compressed_tensor_unsafe_template(const Tensor& compressed_indices,
                                                 const Tensor& plain_indices,
                                                 const Tensor& values,
                                                 IntArrayRef size,
                                                 c10::optional<ScalarType> dtype,
                                                 c10::optional<Layout> layout,
                                                 c10::optional<Device> device,
                                                 c10::optional<bool> pin_memory) {
  Layout layout_ = layout.value_or(required_layout);
  TORCH_CHECK(layout_ == required_layout, "sparse compressed layout must be ",required_layout, " but got ", layout_);
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);
  SparseCsrTensor self = new_compressed_tensor(options);
  get_sparse_csr_impl(self)->set_member_tensors(compressed_indices, plain_indices, values, size);
  return self;
}

#define SPARSE_COMPRESSED_TENSOR_UNSAFE(KIND, REQUIRED_LAYOUT)          \
  Tensor _sparse_##KIND##_tensor_unsafe(const Tensor& compressed_indices, \
                                        const Tensor& plain_indices,    \
                                        const Tensor& values,           \
                                        IntArrayRef size,               \
                                        c10::optional<ScalarType> dtype, \
                                        c10::optional<Layout> layout,   \
                                        c10::optional<Device> device,   \
                                        c10::optional<bool> pin_memory) { \
    return _sparse_compressed_tensor_unsafe_template<REQUIRED_LAYOUT>(compressed_indices, plain_indices, values, size, dtype, layout, device, pin_memory); \
  }

SPARSE_COMPRESSED_TENSOR_UNSAFE(csr, kSparseCsr);
SPARSE_COMPRESSED_TENSOR_UNSAFE(csc, kSparseCsc);
SPARSE_COMPRESSED_TENSOR_UNSAFE(bsr, kSparseBsr);
SPARSE_COMPRESSED_TENSOR_UNSAFE(bsc, kSparseBsc);

DimVector _estimate_sparse_compressed_tensor_size(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    Layout layout) {
  const int block_ndim = AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(layout, "estimate_sparse_compressed_tensor_size", [&] { return 0; }, [&] { return 2; });
  const int base_ndim = 2;  // corresponds to compressed and plain indices
  const int batch_ndim = compressed_indices.dim() - 1;
  const std::string compressed_indices_name = compressedIndicesName(layout);
  const std::string plain_indices_name = plainIndicesName(layout);
  TORCH_CHECK(
              batch_ndim >= 0,
              compressed_indices_name, " must have dimensionality >= 1 but got ", compressed_indices.dim());
  TORCH_CHECK(
              compressed_indices.dim() == plain_indices.dim(),
              compressed_indices_name, " and ", plain_indices_name, " dimensionalities must be equal but got ",
              compressed_indices.dim(), " and ", plain_indices.dim(), ", respectively");
  const int dense_ndim = values.dim() - batch_ndim - block_ndim - 1;
  TORCH_CHECK(
              dense_ndim >= 0,
              "values must have dimensionality > sum of batch and block dimensionalities (=",
              batch_ndim, " + ", block_ndim, ") but got ", values.dim());
  DimVector blocksize{
                      (block_ndim == 2 ? std::max<int64_t>(1, values.size(batch_ndim + 1)) : 1),
                      (block_ndim == 2 ? std::max<int64_t>(1, values.size(batch_ndim + 2)) : 1)
  };
  DimVector size = DimVector(compressed_indices.sizes().slice(0, batch_ndim));
  int64_t ncompressed_dims = (compressed_indices.dim() > 0 && compressed_indices.size(-1) > 0 ? compressed_indices.size(-1) - 1 : 0);
  int64_t nplain_dims = AT_DISPATCH_INTEGRAL_TYPES(plain_indices.scalar_type(), "estimate_sparse_compressed_tensor_size",
                                                   [&]() -> int64_t {
                                                     if (plain_indices.numel() > 0) {
                                                       return plain_indices.max().item<scalar_t>() + 1;
                                                     } else {
                                                       return 0;
                                                     }
                                                   });
  AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(layout, "estimate_sparse_compressed_tensor_size",
      [&]{
        size.push_back(ncompressed_dims * blocksize[0]);
        size.push_back(nplain_dims * blocksize[1]);
      },
      [&]{
        size.push_back(nplain_dims * blocksize[0]);
        size.push_back(ncompressed_dims * blocksize[1]);
      });
  for (int i=0; i<dense_ndim; i++) {
    int64_t j = batch_ndim + 1 + base_ndim + i;
    size.push_back((j < values.dim() ? values.size(j) : 1));
  }
  TORCH_CHECK(
              static_cast<int>(size.size()) == batch_ndim + base_ndim + dense_ndim,
              "tensor dimensionality must be sum of batch, base, and dense dimensionalites (=",
              batch_ndim, " + ", base_ndim, " + ", dense_ndim, ") but got ", size.size());
  return size;
}

// TODO: This constructor should probably use an ATen abstract method in order
// to make autograd dispatch available for the CSR constructor. See the relevant
// note in native_functions.yaml.
Tensor sparse_compressed_tensor(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {

  if (!layout) {
    AT_ERROR("sparse_compressed_tensor expected sparse compressed tensor layout but got none");
  }
  Layout layout_ = layout.value();
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout_, "sparse_compressed_tensor", [&]{});

  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);

  _validate_sparse_compressed_tensor_args_worker(compressed_indices, plain_indices, values, size, layout_);

  return at::native::_sparse_compressed_tensor_unsafe(
      compressed_indices,
      plain_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

Tensor sparse_compressed_tensor(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {

  if (!layout) {
    AT_ERROR("sparse_compressed_tensor expected sparse compressed tensor layout but got none");
  }
  Layout layout_ = layout.value();
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout_, "sparse_compressed_tensor", [&]{});

  DimVector size = _estimate_sparse_compressed_tensor_size(compressed_indices, plain_indices, values, layout_);

  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);

  _validate_sparse_compressed_tensor_args_worker(compressed_indices, plain_indices, values, size, layout_);

  return at::native::_sparse_compressed_tensor_unsafe(
      compressed_indices,
      plain_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

#define SPARSE_COMPRESSED_TENSOR(KIND, REQUIRED_LAYOUT)                 \
  Tensor sparse_##KIND##_tensor(const Tensor& compressed_indices,       \
                                const Tensor& plain_indices,            \
                                const Tensor& values,                   \
                                c10::optional<ScalarType> dtype,        \
                                c10::optional<Layout> layout,           \
                                c10::optional<Device> device,           \
                                c10::optional<bool> pin_memory) {       \
    if (layout) {                                                       \
      TORCH_CHECK(layout.value() == REQUIRED_LAYOUT, "sparse " # KIND " layout must be ", REQUIRED_LAYOUT, " but got ", layout.value()); \
    }                                                                   \
    c10::optional<Layout> layout_(REQUIRED_LAYOUT);                     \
    return at::native::sparse_compressed_tensor(compressed_indices, plain_indices, values, dtype, layout_, device, pin_memory); \
  }                                                                     \
  Tensor sparse_##KIND##_tensor(const Tensor& compressed_indices,       \
                                const Tensor& plain_indices,            \
                                const Tensor& values,                   \
                                IntArrayRef size,                       \
                                c10::optional<ScalarType> dtype,        \
                                c10::optional<Layout> layout,           \
                                c10::optional<Device> device,           \
                                c10::optional<bool> pin_memory) {       \
    if (layout) {                                                       \
      TORCH_CHECK(layout.value() == REQUIRED_LAYOUT, "sparse " # KIND " layout must be ", REQUIRED_LAYOUT, " but got ", layout.value()); \
    }                                                                   \
    c10::optional<Layout> layout_(REQUIRED_LAYOUT);                     \
    return at::native::sparse_compressed_tensor(compressed_indices, plain_indices, values, size, dtype, layout_, device, pin_memory); \
  }

SPARSE_COMPRESSED_TENSOR(csr, kSparseCsr)
SPARSE_COMPRESSED_TENSOR(csc, kSparseCsc)
SPARSE_COMPRESSED_TENSOR(bsr, kSparseBsr)
SPARSE_COMPRESSED_TENSOR(bsc, kSparseBsc)

Tensor empty_symint_sparse_compressed(
    c10::SymIntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<MemoryFormat> optional_memory_format) {
  return at::native::empty_sparse_compressed(c10::asIntArrayRefSlow(size), dtype, layout, device, pin_memory, optional_memory_format);
}

Tensor empty_sparse_compressed(
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<MemoryFormat> optional_memory_format) {
  check_size_nonnegative(size);
  TORCH_CHECK(size.size() >= 2, "torch.empty: Only batched sparse compressed (non-block) tensors are supported, but got size ", size);

  // Strided is the default layout for torch.empty.
  Layout layout_ = layout.value_or(Layout::Strided);

  // torch.empty cannot be used to create blocked tensors because its
  // API lacks a method to specify the block size.
  AT_DISPATCH_SPARSE_COMPRESSED_NONBLOCK_LAYOUTS(layout_, "empty_sparse_compressed", [&]{});

  int64_t nnz = 0;
  auto compressed_indices_size = DimVector(size.slice(0, size.size() - 2));
  auto plain_indices_and_values_size = DimVector(size.slice(0, size.size() - 2));
  compressed_indices_size.push_back(size[compressedDimension(layout_, size)] + 1);
  plain_indices_and_values_size.push_back(nnz);

  TensorOptions options = TensorOptions().dtype(ScalarType::Long).layout(Layout::Strided).device(device).pinned_memory(pin_memory);
  auto compressed_indices = at::empty(compressed_indices_size, options);
  auto plain_indices = at::empty(plain_indices_and_values_size, options);
  auto values = at::empty(plain_indices_and_values_size, options.dtype(dtype));

  return at::native::_sparse_compressed_tensor_unsafe(compressed_indices,
                                                      plain_indices,
                                                      values,
                                                      size,
                                                      dtype,
                                                      layout,
                                                      device,
                                                      pin_memory);
}

const Tensor& resize_sparse_csr_(
    const Tensor& self,
    IntArrayRef size,
    c10::optional<MemoryFormat> optional_memory_format) {
  check_size_nonnegative(size);
  TORCH_CHECK(size.size() >= 2, "torch.resize_: Only batched sparse CSR matrices are supported, but got size ", size);
  TORCH_CHECK(
      self.size(-1) <= size[size.size() - 1],
      "torch.resize_: Resizing columns of sparse CSR tensors to a smaller value is not supported. ",
      "The original number of columns is ",
      self.size(-1),
      " while the requested new number of columns is ", size[size.size() - 1], ".");
  get_sparse_csr_impl(self)->resize_(self._nnz(), size);
  return self;
}

Tensor& copy_sparse_compressed_(Tensor& self, const Tensor& src, bool non_blocking) {
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(self.layout(), "copy_sparse_compressed_", [&]{});
  TORCH_CHECK(
      self.layout() == src.layout(),
      "torch.copy_: copy of sparse compressed tensors having different layouts is not supported.",
      " self layout is ", self.layout(), " and src layout is ", src.layout());
  TORCH_CHECK(
      self._nnz() == src._nnz(),  // actually, values copy allows different shapes as long as operands are broadcastable
      "torch.copy_: only sparse compressed tensors with the same number of specified elements are supported.");
  auto self_compressed_dim = compressedDimension(self.layout(), self.sizes());
  auto src_compressed_dim = compressedDimension(src.layout(), src.sizes());
  auto self_compressed_dims = self.size(self_compressed_dim);
  auto src_compressed_dims = src.size(compressedDimension(src.layout(), src.sizes()));
  if (self_compressed_dim == src_compressed_dim) {
    TORCH_CHECK(self_compressed_dims == src_compressed_dims,
                "torch.copy_: expected shapes of self and src to match along dimension ",
                self_compressed_dim, " for ",
                self.layout(), " layout but the corresponding dimensions of self and src are ",
                self_compressed_dims, " and ", src_compressed_dims, ", respecitvely.");
  } else {
    TORCH_CHECK(self_compressed_dims == src_compressed_dims,
                "torch.copy_: expected shapes of self and src to match along dimensions ",
                self_compressed_dim, " and ", src_compressed_dim, ", respectively, for ",
                self.layout(), " layout but the corresponding dimensions of self and src are ",
                self_compressed_dims, " and ", src_compressed_dims, ", respecitvely.");
  }
  AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(self.layout(), "copy_sparse_compressed_",
                                              [&]{},
                                              [&]{
                                                auto self_values = self.values();
                                                auto src_values = src.values();
                                                auto self_blocksize = DimVector(self_values.sizes().slice(self_values.dim()-2, 2));
                                                auto src_blocksize = DimVector(src_values.sizes().slice(src_values.dim()-2, 2));
                                                TORCH_CHECK(self_blocksize == src_blocksize,
                                                            "torch.copy_: copy of sparse compressed tensors having different block sizes is not supported.",
                                                            " self and src block sizes are ", self_blocksize, " and ", src_blocksize, ", respectivly.");
                                              });
  AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(self.layout(), "copy_sparse_compressed_",
                                            [&]{
                                              self.crow_indices().copy_(src.crow_indices(), non_blocking);
                                              self.col_indices().copy_(src.col_indices(), non_blocking);
                                            },
                                            [&]{
                                              self.ccol_indices().copy_(src.ccol_indices(), non_blocking);
                                              self.row_indices().copy_(src.row_indices(), non_blocking);
                                            });
  self.values().copy_(src.values(), non_blocking);
  return self;
}

// Access members of CSR tensors.
int64_t _nnz_sparse_csr(const SparseCsrTensor& self) {
  return get_sparse_csr_impl(self)->nnz();
}

Tensor values_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->values().alias();
}

Tensor crow_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_ROW_COMPRESSED_LAYOUTS(self.layout(),
                                                   "crow_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->compressed_indices().alias(); });
}

Tensor col_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_ROW_COMPRESSED_LAYOUTS(self.layout(),
                                                   "col_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->plain_indices().alias(); });
}

Tensor ccol_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_COL_COMPRESSED_LAYOUTS(self.layout(),
                                                   "ccol_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->compressed_indices().alias(); });
}

Tensor row_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_COL_COMPRESSED_LAYOUTS(self.layout(),
                                                   "row_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->plain_indices().alias(); });
}

int64_t sparse_dim_sparse_csr(const SparseCsrTensor& self) {
  return get_sparse_csr_impl(self)->sparse_dim();
}

int64_t dense_dim_sparse_csr(const SparseCsrTensor& self) {
  return get_sparse_csr_impl(self)->dense_dim();
}

bool _is_same_size_as_sparse_csr(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  return self.sizes().equals(src.sizes());
}

const SparseCsrTensor& resize_as_sparse_csr_(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  TORCH_CHECK(
      src.is_sparse_csr() && self.is_sparse_csr(),
      "resize_as_sparse_csr_: layout for self and src must be sparse_csr but got ",
      self.layout(),
      " for self, and ",
      src.layout(),
      " for src");
  if (!_is_same_size_as_sparse_csr(self, src)) {
    get_sparse_csr_impl(self)->resize_as_sparse_csr_tensor_(src);
  }
  return self;
}

SparseCsrTensor clone_sparse_compressed(
                                        const SparseCsrTensor& self,
                                        c10::optional<c10::MemoryFormat> optional_memory_format) {
  TORCH_CHECK(
      !optional_memory_format.has_value(),
      "unsupported memory format option ",
      optional_memory_format.value());
  TensorOptions options = self.options();
  auto compressed_indices = AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(self.layout(),
                                                                      "clone_sparse_compressed",
                                                                      [&]{ return self.crow_indices(); },
                                                                      [&]{ return self.ccol_indices(); });
  auto plain_indices = AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(self.layout(),
                                                                 "clone_sparse_compressed",
                                                                 [&]{ return self.col_indices(); },
                                                                 [&]{ return self.row_indices(); });
  return at::native::_sparse_compressed_tensor_unsafe(
                                                      compressed_indices.clone(),
                                                      plain_indices.clone(),
                                                      self.values().clone(),
                                                      self.sizes(),
                                                      optTypeMetaToScalarType(options.dtype_opt()),
                                                      options.layout_opt(),
                                                      options.device_opt(),
                                                      options.pinned_memory_opt());
}

Tensor empty_like_sparse_csr(
    const Tensor& self,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<c10::MemoryFormat> optional_memory_format) {
  TensorOptions options_ = TensorOptions().dtype(dtype).layout(layout).device(device).pinned_memory(pin_memory);
  TensorOptions options =
      self.options()
          .merge_in(options_)
          .merge_memory_format(optional_memory_format);

  if (options.layout() == kSparseCsr) {
    auto result = at::native::_sparse_csr_tensor_unsafe(
        self.crow_indices().clone(),
        self.col_indices().clone(),
        at::empty(self.values().sizes(), options.layout(kStrided)),
        self.sizes(),
        optTypeMetaToScalarType(options.dtype()),
        self.layout(),
        options.device());
    return result;
  } else if (options.layout() == kStrided) {
    return at::native::empty_like(self, dtype, layout, device, pin_memory, optional_memory_format);
  } else {
    TORCH_CHECK(false, "Layout ", options.layout(), " is not supported");
  }
}

Tensor select_sparse_csr(const Tensor& self, int64_t dim, int64_t index) {
  TORCH_CHECK(
      self.layout() == kSparseCsr || self.layout() == kSparseBsr,
      "select(): currently only supports the SparseCsr and SparseBsr layout.");
  TORCH_CHECK_INDEX(
      self.dim() != 0, "select() cannot be applied to a 0-dim tensor.");
  dim = maybe_wrap_dim(dim, self.dim());
  auto size = self.size(dim);
  if (index < -size || index >= size) {
    TORCH_CHECK_INDEX(
        false,
        "select(): index ",
        index,
        " out of range for tensor of size ",
        self.sizes(),
        " at dimension ",
        dim);
  }
  if (index < 0) {
    index += size;
  }

  TORCH_INTERNAL_ASSERT(dim >= 0 && dim < self.dim());

  auto new_sizes = DimVector(self.sizes());
  new_sizes.erase(new_sizes.begin() + dim);
  auto options = self.options();

  // Selecting batch dimension
  if (dim < self.dim() - 2) {
    if (self.layout() == kSparseBsr) {
      return at::native::_sparse_bsr_tensor_unsafe(
          self.crow_indices().select(dim, index),
          self.col_indices().select(dim, index),
          self.values().select(dim, index),
          new_sizes,
          optTypeMetaToScalarType(options.dtype_opt()),
          options.layout_opt(),
          options.device_opt(),
          options.pinned_memory_opt());
    }
    return at::native::_sparse_csr_tensor_unsafe(
        self.crow_indices().select(dim, index),
        self.col_indices().select(dim, index),
        self.values().select(dim, index),
        new_sizes,
        optTypeMetaToScalarType(options.dtype_opt()),
        options.layout_opt(),
        options.device_opt(),
        options.pinned_memory_opt());
  } else {
    TORCH_CHECK(
        self.is_sparse_csr(),
        "select(): selecting non-batch dimensions is currently only supported for CSR tensors.");
    TORCH_CHECK(
        self.dim() == 2,
        "select(): selecting rows or columns is not implemented for batched sparse CSR tensors.")
    // Converting to COO and calling select is slighly slower than operating on
    // the CSR indices directly for constructing a COO vector, however current
    // version is more readable and easier to understand.
    return self.to_sparse().select(dim, index);
  }
}

} // namespace native
} // namespace at

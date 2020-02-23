import math
import numpy as np

from   chainer.utils import type_check

from   chainer_compiler.elichika.typing.ext_functions_utils import *
from   chainer_compiler.elichika.typing.types               import *
from   chainer_compiler.elichika.typing.pytorch.utils       import *


class ty_TorchIdentical():
    def __init__(self, ndim_min=None):
        self.ndim_min = ndim_min

    def __call__(self, ty_args, ty_kwargs):
        x_type = ty_args[0]
        assert isinstance(x_type, TyTensor)
        assert x_type.dtype.kind == 'f'
        if self.ndim_min:
            assert x_type.ndim >= self.ndim_min
        return copy_ty(x_type)

    def nn(self, _, ty_args, ty_kwargs):
        return self(ty_args, ty_kwargs)

# Tensors
## Creation Ops

# TODO: Unify with NumpyArray
class ty_TorchTensor():
    def __call__(self, ty_args, ty_kwargs):
        x_type, = ty_args
        default_dtype = self.get_element_dtype(x_type)
        dtype, lacks_dtype = get_kwarg(ty_kwargs, 'dtype', default_dtype)
        assert not lacks_dtype, "torch.tensor: dtype couldn't inferred"

        return TyNdarray(dtype,
                shape=self.calculate_shape(x_type))

    def calculate_shape(self, x_type):
        if not isinstance(x_type, TySequence):
            return ()
        if not x_type.is_fixed_len:
            return (None,)

        x_tys = x_type.get_tys()

        if isinstance(x_tys[0], TySequence):
            list_lengths = [t.size() if t.is_fixed_len else None for t in x_tys]
            list_lengths_nonnull = [l for l in list_lengths if l is not None]

            # for example, we will not accept np.array([[1,2,3], [4,5]])
            assert all_same(list_lengths_nonnull), \
                    "numpy.array: incompatible list length"

        return (len(x_tys),) + self.calculate_shape(x_tys[0])

    def get_element_dtype(self, ty):
        # get element dtype of nested TySequence
        if isinstance(ty, TySequence):
            return self.get_element_dtype(ty.get())
        return tyobj2dtype(ty)


class ty_TorchTensorOfShape():
    def __call__(self, ty_args, ty_kwargs):
        for ty in ty_args:
            unify(ty, TyInt())

        # TODO: use global default dtype
        dtype, lacks_dtype = get_kwarg(ty_kwargs, 'dtype', np.dtype('float32'))
        assert not lacks_dtype

        shape = wrap_shape([extract_value_from_ty(ty) for ty in ty_args])
        return TyNdarray(dtype, shape=shape)


## Indexing, Slicing, Joining, Mutating Ops

class ty_TorchCat():
    def __init__(self, dim=None):
        self.dim = dim

    def __call__(self, ty_args, ty_kwargs):
        xs_type, = ty_args
        assert isinstance(xs_type, TySequence)
        self.dim, lacks_dim = get_kwarg(ty_kwargs, 'dim', default=0)

        if lacks_value(xs_type) or lacks_dim:
            x_type = xs_type.get()
            return TyTorchTensor(x_type.dtype, ndim=x_type.ndim)

        self.check_type_forward(type_check.Variable(xs_type, 'xs'))
        return self.infer_return(xs_type)

    def check_type_forward(self, in_types):
        type_check.expect(in_types.size() > 0)
        type_check.expect(in_types[0].ndim >
                          type_check.make_variable(self.dim, 'dim'))

        type_check.expect(
            -in_types[0].ndim <= self.dim,
            self.dim < in_types[0].ndim
        )
        ndim = type_check.eval(in_types[0].ndim)
        dim = self.dim % ndim
        for i in range(1, type_check.eval(in_types.size())):
            type_check.expect(
                in_types[0].dtype == in_types[i].dtype,
                in_types[0].ndim == in_types[i].ndim,
            )
            for d in range(0, ndim):
                if d == dim:
                    continue
                type_check.expect(in_types[0].shape[d] == in_types[i].shape[d])

    def infer_return(self, xs_type):
        ret_shape = list(xs_type[0].shape)
        ret_shape[self.dim] = sum([x_type.shape[self.dim] for x_type in xs_type])
        return TyTorchTensor(dtype=xs_type[0].dtype, shape=ret_shape)


class ty_TorchChunk():
    def __call__(self, ty_args, ty_kwargs):
        x_type, chunk_type = ty_args
        assert isinstance(chunk_type, TyNum)
        chunks = chunk_type.value

        self.dim, lacks_dim = get_kwarg(ty_kwargs, 'dim', default=0)
        assert not lacks_dim
        return self.infer_return(x_type, chunks)

    def infer_return(self, x_type, chunks):
        ret_shape = list(x_type.shape)
        if chunks is None:
            ret_shape[self.dim] = None
            return TyTuple(TyTorchTensor(x_type.dtype, shape=ret_shape))

        # TODO(momohatt): Handle cases where dim is not divisible by chunks
        ret_shape[self.dim] = ret_shape[self.dim] // chunks
        return TyTuple([TyTorchTensor(x_type.dtype, shape=ret_shape)
            for _ in range(chunks)])


class ty_TorchReshape():
    def __call__(self, ty_args, ty_kwargs):
        x_type, shape_type = ty_args
        assert isinstance(shape_type, TySequence)
        assert shape_type.is_fixed_len

        self.shape = extract_value_from_ty(shape_type)
        return self.infer_return(x_type)

    def infer_return(self, x_type):
        ret_shape = calculate_reshape(x_type.shape, self.shape)
        return TyTorchTensor(x_type.dtype, shape=ret_shape)


class ty_TorchSplit():
    def __call__(self, ty_args, ty_kwargs):
        x_type, split_size_or_sections_type = ty_args
        self.dim, lacks_dim = get_kwarg(ty_kwargs, 'dim', default=0)

        # TODO: Handle cases where lacks_dim = True

        if isinstance(split_size_or_sections_type, TyNum):
            size = split_size_or_sections_type.value
            assert size is None or size > 0
            return self.infer_return_size(x_type, size)

        sections_type = split_size_or_sections_type
        assert isinstance(sections_type, TySequence)
        return self.infer_return_sections(x_type, sections_type)

    def infer_return_size(self, x_type, size):
        if size is None:
            if self.dim is None:
                return TyTuple(TyTorchTensor(x_type.dtype, ndim=x_type.ndim))
            ret_shape = list(x_type.shape)
            ret_shape[self.dim] = None
            return TyTuple(TyTorchTensor(x_type.dtype, shape=ret_shape))

        if x_type.shape[self.dim].is_null():
            # TODO
            pass

        n_split = math.ceil(x_type.shape[self.dim].get_value() / size)
        if x_type.shape[self.dim] % size != 0:
            ret_shapes = [list(x_type.shape) for _ in range(n_split)]
            for i in range(n_split - 1):
                ret_shapes[i][self.dim] = size
            ret_shapes[-1][self.dim] = x_type.shape[self.dim] % size
            return TyTuple(
                    [TyTorchTensor(x_type.dtype, shape=shape) for shape in ret_shapes])

        ret_shape = list(x_type.shape)
        ret_shape[self.dim] = size
        return TyTuple(
            [TyTorchTensor(x_type.dtype, shape=ret_shape)] * n_split)

    def infer_return_sections(self, x_type, sections_type):
        if not sections_type.is_fixed_len:
            ret_shape = list(x_type.shape)
            ret_shape[self.dim] = None
            return TyTuple(TyTorchTensor(x_type.dtype, shape=ret_shape))

        sections = extract_value_from_ty(sections_type)
        ret_shapes = [list(x_type.shape) for _ in sections]
        for i, n in enumerate(sections):
            ret_shapes[i][self.dim] = n
        return TyTuple([TyTorchTensor(x_type.dtype, shape=shape)
            for shape in ret_shapes])


class ty_TorchSqueeze():
    def __call__(self, ty_args, ty_kwargs):
        x_type, = ty_args
        self.dim, lacks_dim = get_kwarg(ty_kwargs, 'dim', None)

        if is_incomplete_shape(x_type.shape):
            if lacks_dim or self.dim is None:
                assert False, "torch.squeeze: cannot guess ndim of return type"

        return self.infer_return(x_type)

    def infer_return(self, x_type):
        if self.dim is not None:
            ret_shape = remove_dims(x_type.shape, (self.dim,))
        else:
            ret_shape = [s for s in x_type.shape if s != 1]
        return TyTorchTensor(x_type.dtype, shape=ret_shape)


class ty_TorchStack():
    def __init__(self, dim=None):
        self.dim = dim

    def __call__(self, ty_args, ty_kwargs):
        xs_type, = ty_args
        assert isinstance(xs_type, TySequence)

        if xs_type.is_fixed_len:
            for ty in xs_type.get_tys():
                unify(xs_type.get_ty(), ty)

        self.dim, lacks_dim = get_kwarg(ty_kwargs, 'dim', default=0)

        if lacks_dim:
            x_type = xs_type.get()
            return TyTorchTensor(x_type.dtype, ndim=x_type.ndim + 1)

        self.dim %= xs_type.get().ndim + 1
        return self.infer_return(xs_type)

    def infer_return(self, xs_type):
        ret_shape = list(xs_type.get().shape)
        ret_shape.insert(self.dim, ShapeElem(xs_type.size()))
        return TyTorchTensor(xs_type.get().dtype, shape=ret_shape)


class ty_TorchUnsqueeze():
    def __call__(self, ty_args, ty_kwargs):
        x_type, dim_type = ty_args
        assert isinstance(dim_type, TyNum)
        dim = extract_value_from_ty(dim_type)
        if dim is None:
            return TyTorchTensor(x_type.dtype, ndim=x_type.ndim + 1)

        shape = list(x_type.shape)
        shape.insert(dim, 1)
        return TyTorchTensor(x_type.dtype, shape=shape)


# Math operations
## Pointwise Ops

class ty_TorchArith():
    def __init__(self, fn):
        self.fn = fn

    def __call__(self, ty_args, ty_kwargs):
        x_type, y_type = ty_args
        x, y = generate_dummy_value(x_type), generate_dummy_value(y_type)

        try:
            ty_ret = type_of_value(self.fn(x, y))
        except Exception as e:
            ty_ret = handle_inference_error(e, op.__class__.__name__, node)

        if is_incomplete_shape(x_type.shape) or \
                is_incomplete_shape(y_type.shape):
            ty_ret.shape = (ShapeElem(None),) * ty_ret.ndim
        return ty_ret


## Other operations

class ty_TorchFlatten():
    def __call__(self, ty_args, ty_kwargs):
        input_type, = ty_args
        shape = input_type.shape
        start_dim, _ = get_kwarg(ty_kwargs, 'start_dim', default=0)
        end_dim, _   = get_kwarg(ty_kwargs, 'end_dim', default=-1)

        prefix_shape = shape[:start_dim]
        middle_shape = shape[start_dim:end_dim] + (shape[end_dim],)
        postfix_shape = shape[end_dim:][2:]
        size = size_of_shape(middle_shape)
        out_shape = prefix_shape + (size,) + postfix_shape
        return TyTorchTensor(shape=out_shape, dtype=input_type.dtype)


# torch.Tensor

class ty_TorchView():
    def __call__(self, ty_args, ty_kwargs):
        x_type = ty_args[0]
        shape_type = ty_args[1:]
        assert isinstance(x_type, TyTensor)

        out_shape = wrap_shape([extract_value_from_ty(t) for t in shape_type])
        ret_shape = calculate_reshape(x_type.shape, out_shape)
        return TyTorchTensor(x_type.dtype, shape=ret_shape)

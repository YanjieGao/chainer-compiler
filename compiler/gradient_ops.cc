#include "gradient_ops.h"

#include <map>
#include <memory>
#include <string>

#include <common/log.h>
#include <common/strutil.h>
#include <compiler/graph.h>
#include <compiler/graph_builder.h>
#include <compiler/node.h>
#include <compiler/tensor.h>
#include <compiler/type.h>

namespace oniku {
namespace {

void SetGrad(Graph* graph, Value* y, Value* gy) {
    if (y->grad()) {
        // Accumulate gradients.
        GraphBuilder gb(graph, "SetGrad", y);
        Value* v = gb.Op(Node::kAdd, {y->grad(), gy});
        y->set_grad(v);
    } else {
        y->set_grad(gy);
    }
}

Value* AddGradValue(Graph* graph, Value* v) {
    Value* gv = graph->AddValue("grad@" + v->name());
    SetGrad(graph, v, gv);
    return gv;
}

Value* AddGradOp(Graph* graph, Node::OpType op_type, const std::vector<Value*>& inputs, Value* v, const std::string& base) {
    Value* gv = AddGradValue(graph, v);
    graph->AddNode(op_type, inputs, {gv}, base);
    return gv;
}

#define GRAD_OP(...) AddGradOp(graph, __VA_ARGS__, __func__)

void AddGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    SetGrad(graph, x[0], y[0]->grad());
    SetGrad(graph, x[1], y[0]->grad());
}

void SubGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    SetGrad(graph, x[0], y[0]->grad());
    GRAD_OP(Node::kNeg, {y[0]->grad()}, x[1]);
}

void MulGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kMul, {x[1], y[0]->grad()}, x[0]);
    GRAD_OP(Node::kMul, {x[0], y[0]->grad()}, x[1]);
}

void DivGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    Value* gy = y[0]->grad();
    Value* gx0 = GRAD_OP(Node::kDiv, {gy, x[1]}, x[0]);

    GraphBuilder gb(graph, "DivGrad", x[1]);
    Value* t0 = gb.Op(Node::kNeg, {gx0});
    Value* t1 = gb.Op(Node::kMul, {t0, x[0]});
    GRAD_OP(Node::kDiv, {t1, x[1]}, x[1]);
}

void NegGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kNeg, {y[0]->grad()}, x[0]);
}

void ExpGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kMul, {y[0], y[0]->grad()}, x[0]);
}

void SigmoidGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    // TODO(hamaji): Support non-float values.
    CHECK_EQ(Dtype::kFloat32, x[0]->type().dtype());
    GraphBuilder gb(graph, "SigmoidGrad", x[0]);
    Value* gy = y[0]->grad();
    Value* one = gb.Const(Type(x[0]->type().dtype(), {}), {1.0});
    Value* t0 = gb.Op(Node::kMul, {gy, y[0]});
    Value* t1 = gb.Op(Node::kSub, {one, y[0]});
    GRAD_OP(Node::kMul, {t0, t1}, x[0]);
}

void ReluGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kOnikuxReluGrad, {x[0], y[0]->grad()}, x[0]);
}

void SqrtGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "SqrtGrad", x[0]);
    Value* t0 = gb.Op(Node::kAdd, {y[0], y[0]});
    GRAD_OP(Node::kDiv, {y[0]->grad(), t0}, x[0]);
}

void IdentityGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kIdentity, {y[0]->grad()}, x[0]);
}

void ReshapeGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "ReshapeGrad", x[0]);
    Value* t0 = gb.Op(Node::kShape, {x[0]});
    GRAD_OP(Node::kReshape, {y[0]->grad(), t0}, x[0]);
}

void SelectItemGradFn(Graph* graph, const Node*, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "SelectItemGrad", x[0]);
    Value* t0 = gb.Op(Node::kShape, {x[0]});
    GRAD_OP(Node::kOnikuxSelectItemGrad, {y[0]->grad(), x[1], t0}, x[0]);
}

void ReduceSumGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "ReduceSumGrad", x[0]);
    // TODO(hamaji): Need some check for `axes` and `keepdims`.
    Value* gy = y[0]->grad();
    Value* shape = gb.Op(Node::kShape, {x[0]});
    GRAD_OP(Node::kExpand, {gy, shape}, x[0]);
}

void ReduceMeanGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "ReduceMeanGrad", x[0]);
    // TODO(hamaji): Need some check for `axes` and `keepdims`.
    Value* gy = y[0]->grad();
    Value* shape = gb.Op(Node::kShape, {x[0]});
    // TODO(hamaji): Use GraphBuilder.
    Value* zero = graph->AddConstValue("NATIVE_grad_tmp_zero@" + x[0]->name(), Type(Dtype::kInt64, {}), {0});
    Value* batch_size_int = gb.Op(Node::kGather, {shape, zero});
    Value* batch_size = gb.Op(Node::kCast, {batch_size_int});
    batch_size->producer()->set_to(Dtype::kFloat32);
    Value* divided = gb.Op(Node::kDiv, {gy, batch_size});
    GRAD_OP(Node::kExpand, {divided, shape}, x[0]);
}

void GemmGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    // TODO(hamaji): I'm not sure this function is right. I mean I'm
    // pretty sure something is wrong.
    Value* gy = y[0]->grad();

    // Note bias will be ignored thanks to beta=0.
    {
        GraphBuilder gb(graph, "GemmGrad", x[0]);
        Value* gx0 = nullptr;
        if (node->trans_a()) {
            gx0 = gb.Op(Node::kGemm, {x[1], gy, x[0]});
            gx0->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(node->trans_b())->set_trans_b(true);
        } else {
            gx0 = gb.Op(Node::kGemm, {gy, x[1], x[0]});
            gx0->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(false)->set_trans_b(!node->trans_b());
        }
        Value* shape0 = gb.Op(Node::kShape, {x[0]});
        GRAD_OP(Node::kReshape, {gx0, shape0}, x[0]);
    }

    {
        GraphBuilder gb(graph, "GemmGrad", x[1]);
        Value* gx1 = nullptr;
        if (node->trans_b()) {
            gx1 = gb.Op(Node::kGemm, {gy, x[0], x[1]});
            gx1->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(true)->set_trans_b(node->trans_a());
        } else {
            gx1 = gb.Op(Node::kGemm, {x[0], gy, x[1]});
            gx1->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(!node->trans_a())->set_trans_b(false);
        }
        Value* shape1 = gb.Op(Node::kShape, {x[1]});
        GRAD_OP(Node::kReshape, {gx1, shape1}, x[1]);
    }

    GRAD_OP(Node::kReduceSum, {gy}, x[2])->producer()->set_axes({0})->set_keepdims(false);
}

void ConvGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    Value* gy = y[0]->grad();
    Value* w = x[1];
    // TODO(hamaji): Revisit how we handle shapes.
#if 0
    GRAD_OP(Node::kConvTranspose, {gy, w}, x[0])->producer()
        ->set_strides(node->strides())->set_pads(node->pads());
#else
    {
        GraphBuilder gb(graph, "ConvGrad", x[0]);
        Value* x_shape = gb.Op(Node::kShape, {x[0]});
        GRAD_OP(Node::kOnikuxConvTransposeWithDynamicOutputShape, {gy, w, x_shape}, x[0])
            ->producer()
            ->set_strides(node->strides())
            ->set_pads(node->pads());
    }
#endif
    GRAD_OP(Node::kOnikuxConvGradWeight, {w, x[0], gy}, x[1])->producer()->set_strides(node->strides())->set_pads(node->pads());
    if (x.size() == 3) {
        std::vector<int> axes{{0}};
        CHECK(!node->kernel_shape().empty()) << "ConvGrad with no kernel_shape is not supported yet.";
        for (size_t i = 0; i < node->kernel_shape().size(); ++i) {
            axes.push_back(2 + i);
        }
        GRAD_OP(Node::kReduceSum, {gy}, x[2])->producer()->set_axes(axes)->set_keepdims(false);
    }
}

void MaxPoolGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kOnikuxMaxPoolGrad, {y[0], y[0]->grad()}, x[0]);
}

void AveragePoolGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kOnikuxAveragePoolGrad, {y[0], y[0]->grad()}, x[0]);
}

void LogSoftmaxGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "LogSoftmaxGrad", x[0]);
    // TODO(hamaji): This probably works as is. Test it.
    CHECK_EQ(1, node->axis());

    Value* gy = y[0]->grad();
    Value* sum_val = gb.Op(Node::kReduceSum, {gy});
    sum_val->producer()->set_axes({node->axis()})->set_keepdims(true);
    Value* exp_val = gb.Op(Node::kExp, {y[0]});
    Value* mul_val = gb.Op(Node::kMul, {exp_val, sum_val});
    GRAD_OP(Node::kSub, {gy, mul_val}, x[0]);
}

void SoftmaxGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GraphBuilder gb(graph, "SoftmaxGrad", x[0]);
    Value* gy = y[0]->grad();
    Value* gx = gb.Op(Node::kMul, {y[0], gy});
    Value* sum_val = gb.Op(Node::kReduceSum, {gx});
    sum_val->producer()->set_axes({node->axis()})->set_keepdims(true);
    Value* mul_val = gb.Op(Node::kMul, {y[0], sum_val});
    GRAD_OP(Node::kSub, {gx, mul_val}, x[0]);
}

void BatchNormalizationGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    Value* gx0 = AddGradValue(graph, x[0]);
    Value* gx1 = AddGradValue(graph, x[1]);
    Value* gx2 = AddGradValue(graph, x[2]);
    graph->AddNode(Node::kOnikuxBatchNormalizationGrad, {y[0], y[0]->grad()}, {gx0, gx1, gx2}, __func__);
    Value* zero = graph->AddConstValue("grad_tmp_zero@" + x[0]->name(), Type(x[0]->type().dtype(), {1}), {0.0});
    // No gradients since update should have been done for running mean/variance.
    SetGrad(graph, x[3], zero);
    SetGrad(graph, x[4], zero);
}

void LRNGradFn(Graph* graph, const Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y) {
    GRAD_OP(Node::kOnikuxLRNGrad, {x[0], y[0], y[0]->grad()}, x[0])
            ->producer()
            ->set_alpha(node->alpha())
            ->set_beta(node->beta())
            ->set_bias(node->bias())
            ->set_size(node->size());
}

typedef void (*GradFn)(Graph*, const Node*, const std::vector<Value*>&, const std::vector<Value*>&);

struct GradientFunc {
    int num_inputs;
    int num_outputs;
    GradFn fn;
};

}  // namespace

void AddGradientForNode(Graph* graph, const Node* node) {
    static std::map<Node::OpType, GradientFunc>* s_gradient_funcs;
    if (!s_gradient_funcs) {
        // Leak.
        s_gradient_funcs = new std::map<Node::OpType, GradientFunc>;
        auto register_grad_fn = [](Node::OpType op_type, int num_inputs, int num_outputs, GradFn fn) {
            GradientFunc func;
            func.num_inputs = num_inputs;
            func.num_outputs = num_outputs;
            func.fn = fn;
            CHECK(s_gradient_funcs->emplace(op_type, func).second);
        };

        register_grad_fn(Node::kAdd, 2, 1, &AddGradFn);
        register_grad_fn(Node::kSub, 2, 1, &SubGradFn);
        register_grad_fn(Node::kMul, 2, 1, &MulGradFn);
        register_grad_fn(Node::kDiv, 2, 1, &DivGradFn);
        register_grad_fn(Node::kNeg, 1, 1, &NegGradFn);
        register_grad_fn(Node::kExp, 1, 1, &ExpGradFn);
        register_grad_fn(Node::kSigmoid, 1, 1, &SigmoidGradFn);
        register_grad_fn(Node::kRelu, 1, 1, &ReluGradFn);
        register_grad_fn(Node::kSqrt, 1, 1, &SqrtGradFn);

        register_grad_fn(Node::kIdentity, 1, 1, &IdentityGradFn);
        register_grad_fn(Node::kReshape, 2, 1, &ReshapeGradFn);
        register_grad_fn(Node::kOnikuxSelectItem, 2, 1, &SelectItemGradFn);

        register_grad_fn(Node::kReduceSum, 1, 1, &ReduceSumGradFn);
        register_grad_fn(Node::kReduceMean, 1, 1, &ReduceMeanGradFn);
        register_grad_fn(Node::kGemm, 3, 1, &GemmGradFn);
        register_grad_fn(Node::kConv, -1, 1, &ConvGradFn);
        register_grad_fn(Node::kMaxPool, 1, 1, &MaxPoolGradFn);
        register_grad_fn(Node::kAveragePool, 1, 1, &AveragePoolGradFn);
        register_grad_fn(Node::kLogSoftmax, 1, 1, &LogSoftmaxGradFn);
        register_grad_fn(Node::kSoftmax, 1, 1, &SoftmaxGradFn);

        register_grad_fn(Node::kBatchNormalization, 5, -1, &BatchNormalizationGradFn);
        register_grad_fn(Node::kLRN, 1, 1, &LRNGradFn);

        // TODO(hamaji): Implement dropout.
        register_grad_fn(Node::kDropout, 1, 1, &IdentityGradFn);
    }

    auto found = s_gradient_funcs->find(node->op_type());
    CHECK(found != s_gradient_funcs->end()) << "Gradient not supported: " << node->op_type();
    const GradientFunc& func = found->second;
    if (func.num_inputs >= 0) CHECK_EQ(static_cast<size_t>(func.num_inputs), node->inputs().size());
    if (func.num_outputs >= 0) CHECK_EQ(static_cast<size_t>(func.num_outputs), node->outputs().size());
    func.fn(graph, node, node->inputs(), node->outputs());
}

}  // namespace oniku

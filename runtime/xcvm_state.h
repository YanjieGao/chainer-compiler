#pragma once

#include <stack>
#include <string>
#include <vector>

#include <nonstd/optional.hpp>

#include <chainerx/array.h>

#include <runtime/xcvm.h>
#include <runtime/xcvm.pb.h>
#include <runtime/xcvm_var.h>

namespace oniku {
namespace runtime {

class XCVMOptions;
class XCVMVar;

class XCVMState {
public:
    XCVMState(const XCVMOptions& options, int num_variables, const InOuts& inputs);
    ~XCVMState();

    int pc() const {
        return pc_;
    }
    void set_pc(int pc) {
        pc_ = pc;
    }

    chainerx::Array GetArray(int index);
    nonstd::optional<chainerx::Array> GetOptionalArray(int index);
    void SetArray(int index, const chainerx::Array& value);
    void FreeVar(int index);

    std::vector<chainerx::Array> GetArrayList(const std::vector<int>& index);
    void SetArrayList(const std::vector<int>& index, const std::vector<chainerx::Array>& vars);

    XCVMSequence* CreateSequence(int index);
    XCVMSequence* GetSequence(int index);

    const XCVMOpaque& GetOpaque(int index);
    void SetOpaque(int index, XCVMOpaque* opaque);

    XCVMVar* GetXCVMVar(int index);

    std::string GetVarString(int index);
    std::string GetVarListString(const std::vector<int>& indices);

    void Input(const std::string& name, int index);
    void Output(const std::string& name, int index);

    const InOuts& GetOutputs() {
        return std::move(outputs_);
    }

    void CheckNans(const std::vector<int>& inputs, const std::vector<int>& outputs);
    void CheckInfs(const std::vector<int>& inputs, const std::vector<int>& outputs);

    int trace_level() const {
        return trace_level_;
    }
    bool is_training() const {
        return is_training_;
    }
    bool check_nans() const {
        return check_nans_;
    }
    bool check_infs() const {
        return check_infs_;
    }

    void ShowVariableStatus() const;

private:
    void ReportInvalidInOuts(const std::vector<int>& inputs, const std::vector<int>& outputs);

    int pc_;
    std::vector<std::unique_ptr<XCVMVar>> variables_;
    InOuts inputs_;
    InOuts outputs_;
    int trace_level_ = 0;
    bool is_training_ = false;
    bool check_nans_ = false;
    bool check_infs_ = false;
};

}  // namespace runtime
}  // namespace oniku

#pragma once

#include <string>
#include <vector>

#include "serving/core/agent/code_analysis/CodeAnalysisTypes.h"
#include "utils/json.hpp"

class CodeAnalysisEvidenceStore
{
public:
    size_t AddToolOutput(const std::string &tool_name,
                         const nlohmann::json &tool_input,
                         const std::string &tool_output,
                         const std::string &question,
                         CodeAnalysisQuestionType question_type);

    bool Empty() const;
    size_t Size() const;
    bool HasSourceType(const std::string &source_type) const;
    bool HasFocusedContext() const;

    const std::vector<CodeEvidence> &All() const;
    std::vector<CodeEvidence> Top(size_t limit) const;

private:
    size_t AddEvidence(CodeEvidence evidence);
    std::string BuildWhyRelevant(const std::string &tool_name,
                                 const std::string &question,
                                 CodeAnalysisQuestionType question_type,
                                 const std::string &symbol,
                                 const std::string &path) const;

    std::vector<CodeEvidence> evidence_;
};

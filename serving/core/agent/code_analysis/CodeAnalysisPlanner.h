#pragma once

#include <set>
#include <string>
#include <vector>

#include "serving/core/agent/code_analysis/CodeAnalysisEvidence.h"
#include "serving/core/agent/code_analysis/CodeAnalysisTypes.h"

class CodeAnalysisPlanner
{
public:
    CodeAnalysisPlanner(std::string question,
                        std::vector<std::string> allowed_tools,
                        int max_steps);

    const std::string &question() const;
    CodeAnalysisQuestionType question_type() const;
    const CodeAnalysisQuestionHints &hints() const;

    CodeAnalysisPlanStep NextStep(const CodeAnalysisEvidenceStore &evidence_store,
                                  const std::vector<AgentTraceStep> &trace) const;

private:
    bool IsToolAllowed(const std::string &tool_name) const;
    bool HasToolTrace(const std::vector<AgentTraceStep> &trace, const std::string &tool_name) const;
    bool HasToolInputTrace(const std::vector<AgentTraceStep> &trace,
                           const std::string &tool_name,
                           const nlohmann::json &tool_input) const;

    CodeAnalysisPlanStep PickInitialStep() const;
    CodeAnalysisPlanStep PickFallbackStep(const CodeAnalysisEvidenceStore &evidence_store,
                                          const std::vector<AgentTraceStep> &trace) const;

    std::string question_;
    std::vector<std::string> allowed_tools_;
    int max_steps_ = 0;
    CodeAnalysisQuestionType question_type_ = CodeAnalysisQuestionType::unknown;
    CodeAnalysisQuestionHints hints_;
};

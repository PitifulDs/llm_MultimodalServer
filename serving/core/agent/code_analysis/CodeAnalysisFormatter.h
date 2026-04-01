#pragma once

#include <string>
#include <vector>

#include "serving/core/agent/code_analysis/CodeAnalysisTypes.h"

class CodeAnalysisFormatter
{
public:
    static CodeAnalysisFinalAnswer Build(const std::string &question,
                                         CodeAnalysisQuestionType question_type,
                                         const std::vector<CodeEvidence> &evidence,
                                         const std::vector<AgentTraceStep> &trace);

    static std::string ToText(const CodeAnalysisFinalAnswer &answer);
    static std::string ToStructuredText(const CodeAnalysisFinalAnswer &answer);
};

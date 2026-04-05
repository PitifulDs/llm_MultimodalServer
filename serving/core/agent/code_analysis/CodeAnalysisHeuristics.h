#pragma once

#include <string>
#include <vector>

#include "serving/core/agent/code_analysis/CodeAnalysisTypes.h"

CodeAnalysisQuestionType ClassifyCodeAnalysisQuestion(const std::string &question);
CodeAnalysisQuestionHints ExtractCodeAnalysisHints(const std::string &question);

std::vector<ParsedSearchKbHit> ParseSearchKbHits(const std::string &tool_output);
bool ParseOpenChunkOutput(const std::string &tool_output, ParsedOpenChunk &out);
std::vector<ParsedSearchCodeMatch> ParseSearchCodeMatches(const std::string &tool_output);
bool ParseReadFileOutput(const std::string &tool_output, ParsedFileExcerpt &out);
std::vector<ParsedDocMatch> ParseDocMatches(const std::string &tool_output);
std::vector<ParsedWebSearchHit> ParseWebSearchHits(const std::string &tool_output);
bool ParseFetchedUrlOutput(const std::string &tool_output, ParsedFetchedUrl &out);

bool ToolOutputLooksEmpty(const std::string &tool_output);
std::string TrimCodeAnalysisText(const std::string &text, size_t max_chars);
std::string BuildPrimarySearchQuery(const std::string &question,
                                    const CodeAnalysisQuestionHints &hints);
std::string InferPreferredSearchPath(const std::string &question,
                                     const CodeAnalysisQuestionHints &hints);

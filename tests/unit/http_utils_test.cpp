#include "serving/http/HttpUtils.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#define EXPECT_TRUE(cond)                                                                       \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::cerr << "EXPECT_TRUE failed: " << #cond << " at line " << __LINE__ << "\n"; \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

#define EXPECT_EQ(a, b)                                                                                     \
    do                                                                                                       \
    {                                                                                                        \
        if (!((a) == (b)))                                                                                   \
        {                                                                                                    \
            std::cerr << "EXPECT_EQ failed: " << #a << " vs " << #b << " at line " << __LINE__ << "\n"; \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

int main()
{
    using http_utils::diff_messages;
    using http_utils::finish_reason_to_str;
    using http_utils::is_prefix;
    using http_utils::set_sampling_params;

    std::vector<Message> hist = { {"user", "hi"}, {"assistant", "yo"} };
    std::vector<Message> incoming = { {"user", "hi"}, {"assistant", "yo"}, {"user", "more"} };

    EXPECT_TRUE(is_prefix(hist, incoming));

    auto diff = diff_messages(hist, incoming);
    EXPECT_EQ(diff.size(), 1u);
    EXPECT_EQ(diff[0].role, std::string("user"));
    EXPECT_EQ(diff[0].content, std::string("more"));

    std::vector<Message> unrelated = { {"user", "x"} };
    auto diff2 = diff_messages(hist, unrelated);
    EXPECT_EQ(diff2.size(), 1u);
    EXPECT_EQ(diff2[0].content, std::string("x"));

    EXPECT_EQ(std::string(finish_reason_to_str(FinishReason::stop)), std::string("stop"));
    EXPECT_EQ(std::string(finish_reason_to_str(FinishReason::length)), std::string("length"));
    EXPECT_EQ(std::string(finish_reason_to_str(FinishReason::cancelled)), std::string("cancelled"));
    EXPECT_EQ(std::string(finish_reason_to_str(FinishReason::error)), std::string("error"));

    http_utils::json body;
    body["max_tokens"] = 16;
    body["temperature"] = 0.7;
    body["top_p"] = 0.9;
    body["top_k"] = 40;
    body["repeat_penalty"] = 1.1;

    std::unordered_map<std::string, std::string> params;
    set_sampling_params(body, params);

    EXPECT_EQ(params["max_tokens"], std::to_string(16));
    EXPECT_EQ(params["temperature"], std::to_string(0.7));
    EXPECT_EQ(params["top_p"], std::to_string(0.9));
    EXPECT_EQ(params["top_k"], std::to_string(40));
    EXPECT_EQ(params["repeat_penalty"], std::to_string(1.1));

    std::cout << "http_utils_test passed\n";
    return 0;
}

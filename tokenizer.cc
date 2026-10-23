#include "tokenizer.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

vector<string> Tokenizer::tokenize(const string& input) {
    vector<string> tokens;
    string current;

    char quote = '\0';
    bool tokenStarted = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        // Handle characters inside quoted text.
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current += c;
            }

            continue;
        }

        // Start a quoted section.
        if (c == '"' || c == '\'') {
            quote = c;
            tokenStarted = true;
            continue;
        }

        // Finish a token at whitespace.
        if (isspace(static_cast<unsigned char>(c))) {
            if (tokenStarted) {
                tokens.push_back(current);
                current.clear();
                tokenStarted = false;
            }

            continue;
        }

        // Recognize combined output and error redirection.
        if (input.compare(i, 3, ">>&") == 0) {
            if (tokenStarted) {
                tokens.push_back(current);
                current.clear();
                tokenStarted = false;
            }

            tokens.push_back(">>&");
            i += 2;
            continue;
        }

        // Recognize error redirection.
        if (input.compare(i, 2, "2>") == 0) {
            if (tokenStarted) {
                tokens.push_back(current);
                current.clear();
                tokenStarted = false;
            }

            tokens.push_back("2>");
            ++i;
            continue;
        }

        // Recognize append redirection.
        if (input.compare(i, 2, ">>") == 0) {
            if (tokenStarted) {
                tokens.push_back(current);
                current.clear();
                tokenStarted = false;
            }

            tokens.push_back(">>");
            ++i;
            continue;
        }

        // Recognize individual shell operators.
        if (c == '|' || c == '<' || c == '>' || c == '&') {
            if (tokenStarted) {
                tokens.push_back(current);
                current.clear();
                tokenStarted = false;
            }

            tokens.push_back(string(1, c));
            continue;
        }

        // Add ordinary characters to the current token.
        current += c;
        tokenStarted = true;
    }

    // Reject input containing an unmatched quote.
    if (quote != '\0') {
        throw invalid_argument("unmatched quotation mark");
    }

    // Save the final token, including an empty quoted argument.
    if (tokenStarted) {
        tokens.push_back(current);
    }

    return tokens;
}

#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <string>
#include <vector>

using namespace std;

class Tokenizer {
public:
    static vector<string> tokenize(const string& input);
};

#endif

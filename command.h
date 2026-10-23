#ifndef COMMAND_H
#define COMMAND_H

#include <string>
#include <vector>
#include <sys/types.h>

using namespace std;

// Represents one simple command, such as:
// ls -l
// echo Hello
// grep main file.txt

struct SimpleCommand {
    vector<string> _arguments;

    void insertArgument(const string& argument);
};

// Represents a complete shell command.
// It can contain one simple command or multiple commands connected by pipes.

struct Command {
    vector<SimpleCommand*> _simpleCommands;

    string* _outFile;
    string* _inputFile;
    string* _errFile;

    bool _append;
    bool _background;

    Command();

    void insertSimpleCommand(SimpleCommand* simpleCommand);

    void clear();

    void parse();

    void execute();
};

#endif

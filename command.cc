#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <glob.h>
#include <errno.h>
#include <vector>
#include <string>
#include <iostream>
#include <set>
#include "command.h"
#include "tokenizer.h"

namespace
{
    std::set<pid_t> backgroundPids;

    std::string timestampNow()
    {
        time_t now = time(NULL);
        struct tm localTime;

        if (localtime_r(&now, &localTime) == NULL)
            return "unknown-time";

        char buffer[64];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                 &localTime);
        return buffer;
    }

    void logChildTermination(pid_t pid, int status)
    {
        FILE *file = fopen("myshell.log", "a");
        if (file == NULL)
        {
            perror("myshell.log");
            return;
        }

        fprintf(file, "%s PID %ld ",
                timestampNow().c_str(), static_cast<long>(pid));

        if (WIFEXITED(status))
            fprintf(file, "exited with status %d\n",
                    WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(file, "terminated by signal %d\n",
                    WTERMSIG(status));
        else
            fprintf(file, "changed state\n");

        fclose(file);
    }

    void reapBackgroundChildren()
    {
        for (std::set<pid_t>::iterator it = backgroundPids.begin();
             it != backgroundPids.end();)
        {
            int status;
            pid_t result = waitpid(*it, &status, WNOHANG);

            if (result == *it)
            {
                logChildTermination(result, status);
                it = backgroundPids.erase(it);
            }
            else if (result == -1 && errno == ECHILD)
            {
                it = backgroundPids.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool expandWildcard(SimpleCommand *simple,
                        const std::string &argument)
    {
        if (argument.find('*') == std::string::npos &&
            argument.find('?') == std::string::npos)
            return false;

        glob_t matches = {};
        int result = glob(argument.c_str(), 0, NULL, &matches);

        if (result == 0 && matches.gl_pathc > 0)
        {
            for (size_t i = 0; i < matches.gl_pathc; ++i)
                simple->insertArgument(strdup(matches.gl_pathv[i]));

            globfree(&matches);
            return true;
        }

        globfree(&matches);
        return false;
    }

    bool isRedirection(TokenType type)
    {
        return type == TOKEN_REDIRECT ||
               type == TOKEN_APPEND ||
               type == TOKEN_INPUT ||
               type == TOKEN_ERROR ||
               type == TOKEN_REDIRECT_AND_ERROR;
    }

    void setFilename(char **destination, const std::string &value)
    {
        free(*destination);
        *destination = strdup(value.c_str());
    }

    void redirectFile(const char *filename, int target, int flags)
    {
        int fd = open(filename, flags, 0644);

        if (fd == -1)
        {
            perror(filename);
            _exit(1);
        }

        if (dup2(fd, target) == -1)
        {
            perror("dup2");
            close(fd);
            _exit(1);
        }

        close(fd);
    }
}

void parse(std::vector<Token> &tokens)
{
    Command &command = Command::_currentCommand;
    command.clear();

    SimpleCommand *simple = new SimpleCommand();
    bool error = false;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const Token &token = tokens[i];

        if (token.type == TOKEN_EOF)
            break;

        if (isRedirection(token.type))
        {
            if (i + 1 >= tokens.size() ||
                tokens[i + 1].type == TOKEN_EOF ||
                isRedirection(tokens[i + 1].type) ||
                tokens[i + 1].type == TOKEN_PIPE ||
                tokens[i + 1].type == TOKEN_BACKGROUND)
            {
                std::cerr << "Error: missing or invalid redirection filename.\n";
                error = true;
                break;
            }

            std::string filename = tokens[++i].value;

            if (token.type == TOKEN_INPUT)
                setFilename(&command._inputFile, filename);
            else if (token.type == TOKEN_ERROR)
                setFilename(&command._errFile, filename);
            else
            {
                setFilename(&command._outFile, filename);
                command._append =
                    token.type == TOKEN_APPEND ||
                    token.type == TOKEN_REDIRECT_AND_ERROR;
                if (token.type == TOKEN_REDIRECT_AND_ERROR)
                    command._out_error = 1;
            }

            continue;
        }

        if (token.type == TOKEN_PIPE)
        {
            if (simple->_numberOfArguments == 0)
            {
                std::cerr << "Error: missing command before pipe.\n";
                error = true;
                break;
            }

            command.insertSimpleCommand(simple);
            simple = new SimpleCommand();
            continue;
        }

        if (token.type == TOKEN_BACKGROUND)
        {
            command._background = 1;
            continue;
        }

        if (token.value.empty())
            continue;

        if (!expandWildcard(simple, token.value))
            simple->insertArgument(strdup(token.value.c_str()));
    }

    if (!error && simple->_numberOfArguments > 0)
        command.insertSimpleCommand(simple);
    else
    {
        for (int i = 0; i < simple->_numberOfArguments; ++i)
            free(simple->_arguments[i]);

        free(simple->_arguments);
        delete simple;
    }

    if (error)
    {
        command.clear();
        return;
    }

    if (command._numberOfSimpleCommands == 0 &&
        (command._outFile || command._inputFile || command._errFile))
    {
        std::cerr << "Error: redirection requires a command.\n";
        command.clear();
    }
}

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = static_cast<char **>(
        malloc(_numberOfAvailableArguments * sizeof(char *)));

    if (_arguments)
        _arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfArguments + 1 >= _numberOfAvailableArguments)
    {
        _numberOfAvailableArguments *= 2;
        char **arguments = static_cast<char **>(
            realloc(_arguments,
                    _numberOfAvailableArguments * sizeof(char *)));

        if (!arguments)
        {
            perror("realloc");
            free(argument);
            return;
        }

        _arguments = arguments;
    }

    _arguments[_numberOfArguments++] = argument;
    _arguments[_numberOfArguments] = NULL;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 5;
    _numberOfSimpleCommands = 0;
    _simpleCommands = static_cast<SimpleCommand **>(
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *)));

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfSimpleCommands >= _numberOfAvailableSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;
        SimpleCommand **commands = static_cast<SimpleCommand **>(
            realloc(_simpleCommands,
                    _numberOfAvailableSimpleCommands *
                    sizeof(SimpleCommand *)));

        if (!commands)
        {
            perror("realloc");
            return;
        }

        _simpleCommands = commands;
    }

    _simpleCommands[_numberOfSimpleCommands++] = simpleCommand;
}

void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; ++i)
    {
        SimpleCommand *simple = _simpleCommands[i];

        for (int j = 0; j < simple->_numberOfArguments; ++j)
            free(simple->_arguments[j]);

        free(simple->_arguments);
        delete simple;
    }

    _numberOfSimpleCommands = 0;

    free(_outFile);
    free(_inputFile);
    free(_errFile);

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::print()
{
    printf("\n\n              COMMAND TABLE\n\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; ++i)
    {
        printf("  %-3d ", i);

        for (int j = 0;
             j < _simpleCommands[i]->_numberOfArguments; ++j)
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);

        printf("\n");
    }

    printf("\n  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n",
           _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default",
           _errFile ? _errFile : "default",
           _out_error ? (_outFile ? _outFile : "YES") : "default",
           _background ? "YES" : "NO");
    printf("\n\n");
}

void Command::execute()
{
    if (_numberOfSimpleCommands == 0)
    {
        clear();
        return;
    }

    char *first = _simpleCommands[0]->_arguments[0];

    if (_numberOfSimpleCommands == 1 && strcmp(first, "exit") == 0)
    {
        clear();
        printf("Good bye!!\n");
        exit(0);
    }

    if (_numberOfSimpleCommands == 1 && strcmp(first, "cd") == 0)
    {
        char *directory = _simpleCommands[0]->_arguments[1];
        if (!directory)
            directory = getenv("HOME");

        if (!directory || chdir(directory) == -1)
            perror("cd");

        clear();
        return;
    }

    std::vector<pid_t> children;
    int previousRead = -1;
    bool failed = false;

    for (int i = 0; i < _numberOfSimpleCommands; ++i)
    {
        int pipefd[2] = {-1, -1};

        if (i < _numberOfSimpleCommands - 1 && pipe(pipefd) == -1)
        {
            perror("pipe");
            failed = true;
            break;
        }

        pid_t pid = fork();

        if (pid == -1)
        {
            perror("fork");
            if (pipefd[0] != -1) close(pipefd[0]);
            if (pipefd[1] != -1) close(pipefd[1]);
            failed = true;
            break;
        }

        if (pid == 0)
        {
            signal(SIGINT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            if (previousRead != -1)
            {
                if (dup2(previousRead, STDIN_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }
            else if (i == 0 && _inputFile)
                redirectFile(_inputFile, STDIN_FILENO, O_RDONLY);

            if (pipefd[1] != -1)
            {
                if (dup2(pipefd[1], STDOUT_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }
            else if (_outFile)
            {
                int flags = O_WRONLY | O_CREAT |
                            (_append ? O_APPEND : O_TRUNC);
                redirectFile(_outFile, STDOUT_FILENO, flags);

                if (_out_error && dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }

            if (_errFile)
                redirectFile(_errFile, STDERR_FILENO,
                             O_WRONLY | O_CREAT | O_TRUNC);

            if (previousRead != -1) close(previousRead);
            if (pipefd[0] != -1) close(pipefd[0]);
            if (pipefd[1] != -1) close(pipefd[1]);

            execvp(_simpleCommands[i]->_arguments[0],
                   _simpleCommands[i]->_arguments);

            perror(_simpleCommands[i]->_arguments[0]);
            _exit(127);
        }

        children.push_back(pid);
        if (previousRead != -1) close(previousRead);
        if (pipefd[1] != -1) close(pipefd[1]);
        previousRead = pipefd[0];
    }

    if (previousRead != -1)
        close(previousRead);

    if (_background)
    {
        for (size_t i = 0; i < children.size(); ++i)
            backgroundPids.insert(children[i]);

        if (!children.empty())
            printf("[Background process started]\n");
    }
    else
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            int status = 0;
            pid_t result;

            do
                result = waitpid(children[i], &status, 0);
            while (result == -1 && errno == EINTR);

            if (result == -1)
                perror("waitpid");
            else
                logChildTermination(result, status);
        }
    }

    if (failed)
        std::cerr << "Warning: command pipeline may have started only partially.\n";

    clear();
}

void Command::prompt()
{
    signal(SIGINT, SIG_IGN);

    std::string input;

    while (true)
    {
        reapBackgroundChildren();

        std::cout << "myshell> ";
        std::cout.flush();

        if (!std::getline(std::cin, input))
        {
            std::cout << "\n";
            return;
        }

        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
        _currentCommand.execute();

        reapBackgroundChildren();
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand = NULL;

int main()
{
    Command::_currentCommand.prompt();
    return 0;
}

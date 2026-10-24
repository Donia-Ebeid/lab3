/*
 * Original Lab 3 mini-shell template, completed.
 * Existing class names and public methods are preserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <glob.h>
#include <errno.h>
#include <poll.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include <sstream>
#include <set>
#include "command.h"
#include "tokenizer.h"

namespace
{
    volatile sig_atomic_t childSignalWriteFd = -1;
    std::set<pid_t> backgroundPids;

    void childSignalHandler(int)
    {
        int savedErrno = errno;
        int fd = static_cast<int>(childSignalWriteFd);

        if (fd >= 0)
        {
            const char byte = 'C';
            (void)write(fd, &byte, 1);
        }

        errno = savedErrno;
    }

    std::string timestampNow()
    {
        time_t now = time(NULL);
        struct tm localTime;

        if (localtime_r(&now, &localTime) == NULL)
            return "unknown-time";

        char buffer[64];

        if (strftime(buffer, sizeof(buffer),
                     "%Y-%m-%d %H:%M:%S", &localTime) == 0)
            return "unknown-time";

        return buffer;
    }

    void logChildTermination(pid_t pid, int status)
    {
        FILE *logFile = fopen("myshell.log", "a");

        if (logFile == NULL)
        {
            perror("myshell.log");
            return;
        }

        if (WIFEXITED(status))
        {
            fprintf(logFile,
                    "%s PID %ld exited with status %d\n",
                    timestampNow().c_str(),
                    static_cast<long>(pid),
                    WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            fprintf(logFile,
                    "%s PID %ld terminated by signal %d\n",
                    timestampNow().c_str(),
                    static_cast<long>(pid),
                    WTERMSIG(status));
        }
        else
        {
            fprintf(logFile,
                    "%s PID %ld changed state\n",
                    timestampNow().c_str(),
                    static_cast<long>(pid));
        }

        fclose(logFile);
    }

    void reapBackgroundChildren()
    {
        for (std::set<pid_t>::iterator it = backgroundPids.begin();
             it != backgroundPids.end();)
        {
            int status = 0;
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

    bool expandWildcardArgument(SimpleCommand *simple,
                                const char *argument)
    {
        if (strchr(argument, '*') == NULL &&
            strchr(argument, '?') == NULL)
        {
            return false;
        }

        glob_t matches;
        memset(&matches, 0, sizeof(matches));

        int result = glob(argument, 0, NULL, &matches);

        if (result == 0 && matches.gl_pathc > 0)
        {
            for (size_t i = 0; i < matches.gl_pathc; ++i)
            {
                simple->insertArgument(strdup(matches.gl_pathv[i]));
            }

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

    void setFilename(char **destination,
                     const std::string &value)
    {
        if (*destination != NULL)
            free(*destination);

        *destination = strdup(value.c_str());
    }
}

void parse(std::vector<Token> &tokens)
{
    Command &command = Command::_currentCommand;
    command.clear();

    SimpleCommand *simple = new SimpleCommand();
    bool parseError = false;

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
                std::cerr
                    << "Error: missing or invalid redirection filename.\n";

                parseError = true;
                break;
            }

            const std::string filename = tokens[++i].value;

            if (token.type == TOKEN_INPUT)
            {
                setFilename(&command._inputFile, filename);
            }
            else if (token.type == TOKEN_ERROR)
            {
                setFilename(&command._errFile, filename);
            }
            else
            {
                setFilename(&command._outFile, filename);

                command._append =
                    (token.type == TOKEN_APPEND ||
                     token.type == TOKEN_REDIRECT_AND_ERROR) ? 1 : 0;

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
                parseError = true;
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

        bool hasWildcard =
            token.value.find('*') != std::string::npos ||
            token.value.find('?') != std::string::npos;

        if (hasWildcard &&
            expandWildcardArgument(simple, token.value.c_str()))
        {
            continue;
        }

        simple->insertArgument(strdup(token.value.c_str()));
    }

    if (!parseError && simple->_numberOfArguments > 0)
    {
        command.insertSimpleCommand(simple);
    }
    else
    {
        for (int j = 0; j < simple->_numberOfArguments; ++j)
            free(simple->_arguments[j]);

        free(simple->_arguments);
        delete simple;
    }

    if (parseError)
    {
        command.clear();
        return;
    }

    if (command._numberOfSimpleCommands == 0 &&
        (command._outFile != NULL ||
         command._inputFile != NULL ||
         command._errFile != NULL))
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
        malloc(_numberOfAvailableArguments * sizeof(char *))
    );

    if (_arguments != NULL)
        _arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfArguments + 1 >=
        _numberOfAvailableArguments)
    {
        _numberOfAvailableArguments *= 2;

        char **newArguments = static_cast<char **>(
            realloc(
                _arguments,
                _numberOfAvailableArguments * sizeof(char *)
            )
        );

        if (newArguments == NULL)
        {
            perror("realloc");
            free(argument);
            return;
        }

        _arguments = newArguments;
    }

    _arguments[_numberOfArguments] = argument;
    ++_numberOfArguments;
    _arguments[_numberOfArguments] = NULL;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 5;
    _numberOfSimpleCommands = 0;

    _simpleCommands = static_cast<SimpleCommand **>(
        malloc(
            _numberOfAvailableSimpleCommands *
            sizeof(SimpleCommand *)
        )
    );

    _outFile = NULL;
    _inputFile = NULL;
    _errFile = NULL;
    _out_error = 0;
    _background = 0;
    _append = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfSimpleCommands >=
        _numberOfAvailableSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;

        SimpleCommand **newCommands =
            static_cast<SimpleCommand **>(
                realloc(
                    _simpleCommands,
                    _numberOfAvailableSimpleCommands *
                    sizeof(SimpleCommand *)
                )
            );

        if (newCommands == NULL)
        {
            perror("realloc");
            return;
        }

        _simpleCommands = newCommands;
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

    if (_outFile != NULL)
        free(_outFile);

    if (_inputFile != NULL)
        free(_inputFile);

    if (_errFile != NULL)
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
    printf("\n\n");
    printf("              COMMAND TABLE                \n\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; ++i)
    {
        printf("  %-3d ", i);

        for (int j = 0;
             j < _simpleCommands[i]->_numberOfArguments;
             ++j)
        {
            printf("\"%s\" \t",
                   _simpleCommands[i]->_arguments[j]);
        }

        printf("\n");
    }

    printf("\n  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");

    printf("  %-12s %-12s %-12s %-12s %-12s\n",
           _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default",
           _errFile ? _errFile : "default",
           _out_error ?
               (_outFile ? _outFile : "YES") : "default",
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

    const char *first = _simpleCommands[0]->_arguments[0];

    if (_numberOfSimpleCommands == 1 &&
        strcmp(first, "exit") == 0)
    {
        clear();
        printf("Good bye!!\n");
        exit(0);
    }

    if (_numberOfSimpleCommands == 1 &&
        strcmp(first, "cd") == 0)
    {
        char *directory = _simpleCommands[0]->_arguments[1];

        if (directory == NULL)
            directory = getenv("HOME");

        if (directory == NULL || chdir(directory) == -1)
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

        if (i < _numberOfSimpleCommands - 1 &&
            pipe(pipefd) == -1)
        {
            perror("pipe");
            failed = true;
            break;
        }

        pid_t pid = fork();

        if (pid == -1)
        {
            perror("fork");

            if (pipefd[0] != -1)
                close(pipefd[0]);

            if (pipefd[1] != -1)
                close(pipefd[1]);

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
            else if (i == 0 && _inputFile != NULL)
            {
                int fd = open(_inputFile, O_RDONLY);

                if (fd == -1)
                {
                    perror(_inputFile);
                    _exit(1);
                }

                if (dup2(fd, STDIN_FILENO) == -1)
                {
                    perror("dup2");
                    close(fd);
                    _exit(1);
                }

                close(fd);
            }

            if (pipefd[1] != -1)
            {
                if (dup2(pipefd[1], STDOUT_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }
            else if (_outFile != NULL)
            {
                int flags = O_WRONLY | O_CREAT |
                    (_append ? O_APPEND : O_TRUNC);

                int fd = open(_outFile, flags, 0644);

                if (fd == -1)
                {
                    perror(_outFile);
                    _exit(1);
                }

                if (dup2(fd, STDOUT_FILENO) == -1)
                {
                    perror("dup2");
                    close(fd);
                    _exit(1);
                }

                close(fd);

                if (_out_error &&
                    dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
                {
                    perror("dup2");
                    _exit(1);
                }
            }

            if (_errFile != NULL)
            {
                int fd = open(
                    _errFile,
                    O_WRONLY | O_CREAT | O_TRUNC,
                    0644
                );

                if (fd == -1)
                {
                    perror(_errFile);
                    _exit(1);
                }

                if (dup2(fd, STDERR_FILENO) == -1)
                {
                    perror("dup2");
                    close(fd);
                    _exit(1);
                }

                close(fd);
            }

            if (previousRead != -1)
                close(previousRead);

            if (pipefd[0] != -1)
                close(pipefd[0]);

            if (pipefd[1] != -1)
                close(pipefd[1]);

            execvp(
                _simpleCommands[i]->_arguments[0],
                _simpleCommands[i]->_arguments
            );

            perror(_simpleCommands[i]->_arguments[0]);
            _exit(127);
        }

        children.push_back(pid);

        if (previousRead != -1)
            close(previousRead);

        if (pipefd[1] != -1)
            close(pipefd[1]);

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
            {
                result = waitpid(children[i], &status, 0);
            }
            while (result == -1 && errno == EINTR);

            if (result == -1)
                perror("waitpid");
            else
                logChildTermination(result, status);
        }
    }

    if (failed)
    {
        std::cerr
            << "Warning: command pipeline may have started only partially.\n";
    }

    clear();
}

void Command::prompt()
{
    signal(SIGINT, SIG_IGN);

    int signalPipe[2];

    if (pipe(signalPipe) == -1)
    {
        perror("pipe");
        return;
    }

    for (int i = 0; i < 2; ++i)
    {
        int flags = fcntl(signalPipe[i], F_GETFL, 0);

        if (flags != -1)
            (void)fcntl(
                signalPipe[i],
                F_SETFL,
                flags | O_NONBLOCK
            );

        (void)fcntl(signalPipe[i], F_SETFD, FD_CLOEXEC);
    }

    childSignalWriteFd = signalPipe[1];

    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = childSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGCHLD, &action, NULL) == -1)
    {
        perror("sigaction");
        close(signalPipe[0]);
        close(signalPipe[1]);
        childSignalWriteFd = -1;
        return;
    }

    std::string input;

    while (true)
    {
        reapBackgroundChildren();

        std::cout << "myshell> ";
        std::cout.flush();

        bool commandReady = false;

        while (!commandReady)
        {
            struct pollfd fds[2];

            fds[0].fd = STDIN_FILENO;
            fds[0].events = POLLIN;
            fds[0].revents = 0;

            fds[1].fd = signalPipe[0];
            fds[1].events = POLLIN;
            fds[1].revents = 0;

            int result = poll(fds, 2, -1);

            if (result == -1)
            {
                if (errno == EINTR)
                    continue;

                perror("poll");
                close(signalPipe[0]);
                close(signalPipe[1]);
                childSignalWriteFd = -1;
                return;
            }

            if (fds[1].revents & POLLIN)
            {
                char buffer[128];

                while (read(signalPipe[0], buffer, sizeof(buffer)) > 0)
                {
                }

                reapBackgroundChildren();
            }

            if (fds[0].revents & (POLLIN | POLLHUP))
            {
                if (!std::getline(std::cin, input))
                {
                    std::cout << "\n";

                    close(signalPipe[0]);
                    close(signalPipe[1]);
                    childSignalWriteFd = -1;
                    return;
                }

                commandReady = true;
            }

            if (fds[0].revents & (POLLERR | POLLNVAL))
            {
                std::cerr << "Input error.\n";

                close(signalPipe[0]);
                close(signalPipe[1]);
                childSignalWriteFd = -1;
                return;
            }
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

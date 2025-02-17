#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <random>
#include <chrono>
#include <algorithm>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

struct Question {
    std::string content;
    std::vector<std::string> answers;
    int correctAnswer;
    int timeLimit;  // in seconds
};

struct Quiz {
    std::string code;
    std::vector<Question> questions;
    std::map<int, std::string> participants;  // socket -> name
    std::map<int, int> scores;  // socket -> score
    int currentQuestion;
    bool isActive;
    int creatorSocket;
    std::map<int, int> answers;  // socket -> answer
};

class KahootServer {
private:
    int serverSocket;
    std::map<std::string, Quiz> activeQuizzes;
    
    std::string generateQuizCode() {
        static const char charset[] = "0123456789ABCDEF";
        std::string code;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);
        
        for (int i = 0; i < 6; ++i) {
            code += charset[dis(gen)];
        }
        return code;
    }

    void notifyAllParticipants(const std::string& quizCode, const std::string& message, int excludeSocket = -1) {
        auto& quiz = activeQuizzes[quizCode];
        for (const auto& participant : quiz.participants) {
            if (participant.first != excludeSocket) {
                send(participant.first, message.c_str(), message.length(), 0);
                send(participant.first, "\n", 1, 0);
            }
        }
    }

    private:
        std::map<int, std::string> clientQuizCodes;  // socket -> quiz code

    void handleClient(int clientSocket) {
        while (true) {
            char buffer[1024] = {0};
            ssize_t bytesRead = read(clientSocket, buffer, 1024);
            
            if (bytesRead <= 0) {
                // Client disconnected
                std::string quizCode = clientQuizCodes[clientSocket];
                if (!quizCode.empty() && activeQuizzes.find(quizCode) != activeQuizzes.end()) {
                    activeQuizzes[quizCode].participants.erase(clientSocket);
                    activeQuizzes[quizCode].scores.erase(clientSocket);
                    activeQuizzes[quizCode].answers.erase(clientSocket);
                }
                clientQuizCodes.erase(clientSocket);
                close(clientSocket);
                return;
            }

            std::string message(buffer);

            if (message.substr(0, 6) == "CREATE") {
                printf("Creating new quiz for client with socket %d\n", clientSocket);
                std::string code = generateQuizCode();
                Quiz newQuiz;
                newQuiz.code = code;
                newQuiz.isActive = false;
                newQuiz.creatorSocket = clientSocket;
                newQuiz.currentQuestion = -1;
                activeQuizzes[code] = newQuiz;
                clientQuizCodes[clientSocket] = code;
                
                std::string response = "QUIZ_CODE:" + code + "\n";
                send(clientSocket, response.c_str(), response.length(), 0);
            }
            else if (message.substr(0, 4) == "JOIN") {
                std::string code = message.substr(5, 6);
                std::string name = message.substr(12);
                if (name.empty()) {
                    send(clientSocket, "ERROR:Name cannot be empty\n", 27, 0);
                    continue;
                }

                if (activeQuizzes.find(code) == activeQuizzes.end()) {
                    send(clientSocket, "ERROR:Invalid quiz code\n", 24, 0);
                    continue;
                }

                auto& quiz = activeQuizzes[code];
                for (const auto& participant : quiz.participants) {
                    if (participant.second == name) {
                        send(clientSocket, "ERROR:Name already taken\n", 25, 0);
                        handleClient(clientSocket);
                    }
                }
                
                quiz.participants[clientSocket] = name;
                quiz.scores[clientSocket] = 0;
                clientQuizCodes[clientSocket] = code;
                
                std::string response = "JOINED:" + code + "\n";
                send(clientSocket, response.c_str(), response.length(), 0);

                printf("Client with socket %d joined quiz %s\n", clientSocket, code.c_str());
            }
            else if (message.substr(0, 12) == "ADD_QUESTION") {
                std::string code = clientQuizCodes[clientSocket];
                if (code.empty() || activeQuizzes.find(code) == activeQuizzes.end()) {
                    send(clientSocket, "ERROR:Not part of any quiz\n", 27, 0);
                    continue;
                }

                if (activeQuizzes[code].creatorSocket != clientSocket) {
                    send(clientSocket, "ERROR:Only quiz creator can add questions\n", 41, 0);
                    continue;
                }

                try {
                    size_t pos = message.find(':', 13);
                    if (pos == std::string::npos) {
                        send(clientSocket, "ERROR:Invalid question format\n", 30, 0);
                        continue;
                    }
                    std::string content = message.substr(13, pos - 13);
                    printf("Question content: %s\n", content.c_str());

                    Question q;
                    q.content = content;

                    for (int i = 0; i < 4; i++) {
                        size_t nextPos = message.find(':', pos + 1);
                        if (nextPos == std::string::npos) {
                            send(clientSocket, "ERROR:Invalid answer format\n", 28, 0);
                            continue;
                        }
                        printf("Answer %d: %s\n", i, message.substr(pos + 1, nextPos - pos - 1).c_str());
                        q.answers.push_back(message.substr(pos + 1, nextPos - pos - 1));
                        pos = nextPos;
                    }

                    pos = message.find(':', pos + 1);
                    if (pos == std::string::npos) {
                        send(clientSocket, "ERROR:Missing correct answer\n", 29, 0);
                        continue;
                    }
                    q.correctAnswer = std::stoi(message.substr(pos+1, 1));
                    printf("Correct answer index: %d\n", q.correctAnswer);
                    if (q.correctAnswer < 0 || q.correctAnswer >= 4) {
                        send(clientSocket, "ERROR:Invalid correct answer number\n", 38, 0);
                        continue;
                    }

                    pos = message.find(':', pos + 1);
                    if (pos == std::string::npos) {
                        send(clientSocket, "ERROR:Missing time limit\n", 25, 0);
                        continue;
                    }
                    q.timeLimit = std::stoi(message.substr(pos + 1));
                    printf("Time limit: %d\n", q.timeLimit);
                    if (q.timeLimit <= 0) {
                        send(clientSocket, "ERROR:Time limit must be positive\n", 33, 0);
                        continue;
                    }

                    activeQuizzes[code].questions.push_back(q);
                    send(clientSocket, "QUESTION_ADDED\n", 15, 0);
                } catch (const std::exception& e) {
                    send(clientSocket, "ERROR:Invalid question format\n", 30, 0);
                }
            }
            else if (message.substr(0, 5) == "START") {
                std::string code = clientQuizCodes[clientSocket];
                if (!code.empty() && activeQuizzes.find(code) != activeQuizzes.end() && 
                    activeQuizzes[code].creatorSocket == clientSocket) {
                    activeQuizzes[code].isActive = true;
                    activeQuizzes[code].currentQuestion = 0;
                    notifyAllParticipants(code, "Quiz has started!\n");
                    broadcastQuestion(code);
                    startQuestionTimer(code);
                }
            }
            else if (message.substr(0, 6) == "ANSWER") {
                std::string code = clientQuizCodes[clientSocket];
                if (code.empty()) {
                    send(clientSocket, "ERROR:Not part of any quiz\n", 27, 0);
                    continue;
                }
                
                int answer = std::stoi(message.substr(7));
                if (activeQuizzes.find(code) != activeQuizzes.end() && 
                    activeQuizzes[code].isActive) {
                    activeQuizzes[code].answers[clientSocket] = answer;
                    // Add notification for creator
                    notifyCreator(code, activeQuizzes[code].participants[clientSocket], answer);
                    checkAnswers(code);
                }
            }
        }
    }
private:
    void broadcastQuestion(const std::string& quizCode) {
    auto& quiz = activeQuizzes[quizCode];
    if (quiz.currentQuestion >= static_cast<int>(quiz.questions.size())) return;

    auto& question = quiz.questions[quiz.currentQuestion];
    std::string message = "QUESTION:" + question.content;
    for (const auto& answer : question.answers) {
        message += ":" + answer;
    }
    message += ":" + std::to_string(question.timeLimit);

    for (const auto& participant : quiz.participants) {
        send(participant.first, message.c_str(), message.length(), 0);
        send(participant.first, "\n", 1, 0);
    }
}
private:
    void startQuestionTimer(const std::string& quizCode) {
        std::thread([this, quizCode]() {
            auto& quiz = activeQuizzes[quizCode];
            if (quiz.currentQuestion >= static_cast<int>(quiz.questions.size())) return;
            
            int timeLimit = quiz.questions[quiz.currentQuestion].timeLimit;
            std::this_thread::sleep_for(std::chrono::seconds(timeLimit));
            
            if (quiz.isActive && quiz.currentQuestion < static_cast<int>(quiz.questions.size())) {
                printf("Time's up for question %d in quiz %s\n", quiz.currentQuestion, quizCode.c_str());
                checkAnswers(quizCode, 1);
            }
        }).detach();
    }
private:
    void notifyCreator(const std::string& quizCode, const std::string& playerName, int answer) {
        auto& quiz = activeQuizzes[quizCode];
        std::string message = "PLAYER_ANSWER:" + playerName + ":" + std::to_string(answer) + "\n";
        send(quiz.creatorSocket, message.c_str(), message.length(), 0);
    }
public:
    KahootServer(int port) {
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket == -1) {
            std::cerr << "Failed to create socket" << std::endl;
            return;
        }

        int opt = 1;
        if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "setsockopt(SO_REUSEADDR) failed" << std::endl;
            return;
        }

        sockaddr_in serverAddress;
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_addr.s_addr = INADDR_ANY;  // Accept connections from any address
        serverAddress.sin_port = htons(port);

        if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
            std::cerr << "Bind failed" << std::endl;
            return;
        }
        
        // Print all available network interfaces
        printf("Server is listening on all network interfaces:\n");
        printf("localhost:    127.0.0.1:%d\n", port);
        printf("all interfaces: 0.0.0.0:%d\n", port);
        
        if (listen(serverSocket, 10) < 0) {
            std::cerr << "Listen failed" << std::endl;
            return;
        }

        printNetworkInterfaces(port);
    }

    void start() {
        while (true) {
            sockaddr_in clientAddress;
            socklen_t clientAddressLen = sizeof(clientAddress);
            int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddressLen);
            
            if (clientSocket < 0) {
                std::cerr << "Accept failed" << std::endl;
                continue;
            }
            printf("New client connected with socket %d\n", clientSocket);
            std::thread clientThread(&KahootServer::handleClient, this, clientSocket);
            clientThread.detach();
        }
    }

    void checkAnswers(const std::string& quizCode, int force = 0) {
        auto& quiz = activeQuizzes[quizCode];
        if (!quiz.isActive || quiz.currentQuestion >= static_cast<int>(quiz.questions.size())) return;

        auto& question = quiz.questions[quiz.currentQuestion];
        
        // Remove answers from disconnected players
        for (auto it = quiz.answers.begin(); it != quiz.answers.end();) {
            if (quiz.participants.find(it->first) == quiz.participants.end()) {
                it = quiz.answers.erase(it);
            } else {
                ++it;
            }
        }

        int totalPlayers = quiz.participants.size();
        int answeredPlayers = quiz.answers.size();

        printf("Checking answers for quiz %s, question %d\n", quizCode.c_str(), quiz.currentQuestion);
        printf("Total players: %d, Answered players: %d\n", totalPlayers, answeredPlayers);

        // Ensure at least 2/3 of participants have answered
        if (force == 1)
        {
            answeredPlayers = totalPlayers;
        }
        
        printf(answeredPlayers >= (2 * totalPlayers) / 3 ? "Enough answers\n" : "Not enough answers\n");
        if (static_cast<float>(answeredPlayers) / totalPlayers >= 2.0f / 3.0f) {
            for (const auto& answer : quiz.answers) {
                if (answer.second == question.correctAnswer) {
                    quiz.scores[answer.first] += 100;
                }
            }
            
            // Prepare the score update message
            std::string scoreUpdate = "SCORES:";
            for (const auto& participant : quiz.participants) {
                scoreUpdate += participant.second + ":" + std::to_string(quiz.scores[participant.first]) + ";";
            }

            // Send the score update to all participants
            for (const auto& participant : quiz.participants) {
                send(participant.first, scoreUpdate.c_str(), scoreUpdate.length(), 0);
                send(participant.first, "\n", 1, 0);
            }

            quiz.answers.clear();
            quiz.currentQuestion++;

            if (quiz.currentQuestion < static_cast<int>(quiz.questions.size())) {
                broadcastQuestion(quizCode);
            } else {
                notifyAllParticipants(quizCode, "Quiz has ended!");
                quiz.isActive = false;
            }
        } else {
            printf("Waiting for more answers...\n");
        }
    }

    ~KahootServer() {
        close(serverSocket);
    }

    void printNetworkInterfaces(int port) {
        struct ifaddrs *ifaddr, *ifa;
        int family;

        if (getifaddrs(&ifaddr) == -1) {
            perror("getifaddrs");
            return;
        }

        printf("\nAvailable network interfaces:\n");
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL)
                continue;

            family = ifa->ifa_addr->sa_family;
            if (family == AF_INET) { // IPv4
                char host[NI_MAXHOST];
                inet_ntop(AF_INET, 
                         &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                         host, 
                         NI_MAXHOST);
                printf("%s:\t%s:%d\n", ifa->ifa_name, host, port);
            }
        }
        printf("\n");
        freeifaddrs(ifaddr);
    }
};

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port;
    try {
        port = std::stoi(argv[1]);
        if (port < 1024 || port > 65535) {
            printf("Error: Port number must be between 1024 and 65535\n");
            return 1;
        }
    } catch (const std::exception& e) {
        printf("Error: Invalid port number\n");
        return 1;
    }

    KahootServer server(port);
    printf("Server started on port %d\n", port);
    printf("\n");
    printf("Instructions for quiz creator:\n");
    printf("CREATE - Create a new quiz\n");
    printf("ADD_QUESTION:<question>:<answer1>:<answer2>:<answer3>:<answer4>:<correct_answer>:<time_limit>\n");
    printf("START - Start the quiz\n");
    printf("\n");
    printf("Instructions for quiz participants:\n");
    printf("JOIN:<quiz_code>:<name> - Join a quiz\n");
    printf("ANSWER:<answer_number> - Answer current question\n");
    server.start();
    return 0;
}

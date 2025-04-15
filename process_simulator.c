#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>

#define MAX_PROCESSES 10
#define MAX_INSTRUCTIONS 100

typedef enum { STATE_READY, STATE_RUNNING, STATE_BLOCKED, STATE_TERMINATED } State;

typedef struct {
    char operation;
    int intArg;
    char stringArg[256];
} Instruction;

typedef struct {
    Instruction instructions[MAX_INSTRUCTIONS];
    int instructionCount;
} Program;

typedef struct {
    Program *pProgram;
    int programCounter;
    int value;
    int timeSlice;
    int timeSliceUsed;
} Cpu;

typedef struct {
    int processId;
    int parentProcessId;
    Program program;
    unsigned int programCounter;
    int value;
    unsigned int priority;
    State state;
    unsigned int startTime;
    unsigned int timeUsed;
} PcbEntry;

PcbEntry pcbTable[MAX_PROCESSES];
unsigned int timestamp = 0;
Cpu cpu;
int runningState = -1;
int readyQueue[MAX_PROCESSES];
int readyQueueSize = 0;
int blockedQueue[MAX_PROCESSES];
int blockedQueueSize = 0;
double cumulativeTimeDiff = 0;
int numTerminatedProcesses = 0;
int nextProcessId = 1;

void trim(char *str) {
    char *start = str;
    while (isspace(*start)) start++;
    memmove(str, start, strlen(start) + 1);

    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) end--;
    *(end + 1) = '\0';
}

int createProgram(const char *filename, Program *program) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Error opening file %s\n", filename);
        return 0;
    }
    char line[256];
    int lineNum = 0;
    while (fgets(line, sizeof(line), file)) {
        trim(line);
        if (strlen(line) > 0) {
            Instruction *instruction = &program->instructions[program->instructionCount++];
            instruction->operation = toupper(line[0]);
            strcpy(instruction->stringArg, line + 1);
            trim(instruction->stringArg);
            switch (instruction->operation) {
                case 'S':
                case 'A':
                case 'D':
                case 'F':
                    instruction->intArg = atoi(instruction->stringArg);
                    break;
                case 'B':
                case 'E':
                    break;
                case 'R':
                    if (strlen(instruction->stringArg) == 0) {
                        printf("%s:%d - Missing string argument\n", filename, lineNum);
                        fclose(file);
                        return 0;
                    }
                    break;
                default:
                    printf("%s:%d - Invalid operation, %c\n", filename, lineNum, instruction->operation);
                    fclose(file);
                    return 0;
            }
        }
        lineNum++;
    }
    fclose(file);
    return 1;
}

void set(int value) {
    cpu.value = value;
}

void add(int value) {
    cpu.value += value;
}

void decrement(int value) {
    cpu.value -= value;
}

void block() {
    if (runningState != -1) {
        blockedQueue[blockedQueueSize++] = runningState;
        pcbTable[runningState].state = STATE_BLOCKED;
        pcbTable[runningState].programCounter = cpu.programCounter;
        pcbTable[runningState].value = cpu.value;
        runningState = -1;
    }
}

void end() {
    if (runningState != -1) {
        PcbEntry *pcb = &pcbTable[runningState];
        cumulativeTimeDiff += timestamp + 1 - pcb->startTime;
        numTerminatedProcesses++;
        pcb->state = STATE_TERMINATED;
        runningState = -1;
    }
}

void forkProcess(int value) {
    int newPcbIndex = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcbTable[i].state == STATE_TERMINATED) {
            newPcbIndex = i;
            break;
        }
    }
    if (newPcbIndex == -1) {
        printf("No available PCB entry for forking process.\n");
        return;
    }
    PcbEntry *parentPcb = &pcbTable[runningState];
    PcbEntry *newPcb = &pcbTable[newPcbIndex];
    newPcb->processId = nextProcessId++;
    newPcb->parentProcessId = parentPcb->processId;
    newPcb->program = parentPcb->program;
    newPcb->programCounter = parentPcb->programCounter;
    newPcb->value = parentPcb->value;
    newPcb->priority = parentPcb->priority;
    newPcb->state = STATE_READY;
    newPcb->startTime = timestamp;
    newPcb->timeUsed = 0;
    readyQueue[readyQueueSize++] = newPcbIndex;
    parentPcb->programCounter += value;
}

void replace(char *argument) {
    cpu.pProgram->instructionCount = 0;
    if (!createProgram(argument, cpu.pProgram)) {
        cpu.programCounter++;
        return;
    }
    cpu.programCounter = 0;
}

void schedule() {
    if (runningState != -1) return;
    if (readyQueueSize > 0) {
        runningState = readyQueue[0];
        for (int i = 1; i < readyQueueSize; i++) {
            readyQueue[i - 1] = readyQueue[i];
        }
        readyQueueSize--;
        PcbEntry *pcb = &pcbTable[runningState];
        pcb->state = STATE_RUNNING;
        cpu.pProgram = &pcb->program;
        cpu.programCounter = pcb->programCounter;
        cpu.value = pcb->value;
    }
}

void quantum() {
    if (runningState == -1) {
        printf("No processes are running\n");
        ++timestamp;
        return;
    }

    if (cpu.programCounter < cpu.pProgram->instructionCount) {
        Instruction *instruction = &cpu.pProgram->instructions[cpu.programCounter++];
        switch (instruction->operation) {
            case 'S':
                set(instruction->intArg);
                printf("Time: %d, Process %d executed instruction S %d\n", timestamp, runningState, instruction->intArg);
                break;
            case 'A':
                add(instruction->intArg);
                printf("Time: %d, Process %d executed instruction A %d\n", timestamp, runningState, instruction->intArg);
                break;
            case 'D':
                decrement(instruction->intArg);
                printf("Time: %d, Process %d executed instruction D %d\n", timestamp, runningState, instruction->intArg);
                break;
            case 'B':
                block();
                printf("Time: %d, Process %d executed instruction B\n", timestamp, runningState);
                break;
            case 'E':
                end();
                printf("Time: %d, Process %d executed instruction E\n", timestamp, runningState);
                break;
            case 'F':
                forkProcess(instruction->intArg);
                printf("Time: %d, New Process %d created, Process %d continues\n", timestamp, pcbTable[nextProcessId - 1].processId, runningState);
                break;
            case 'R':
                replace(instruction->stringArg);
                printf("Time: %d, Process %d replaced with new program\n", timestamp, runningState);
                break;
        }
    } else {
        printf("End of program reached without E operation\n");
        end();
    }
    ++timestamp;
    schedule();
}

void unblock() {
    if (blockedQueueSize > 0) {
        int processIndex = blockedQueue[0];
        for (int i = 1; i < blockedQueueSize; i++) {
            blockedQueue[i - 1] = blockedQueue[i];
        }
        blockedQueueSize--;
        readyQueue[readyQueueSize++] = processIndex;
        pcbTable[processIndex].state = STATE_READY;
    }
    schedule();
}

void printState() {
    printf("Current system state at time %d:\n", timestamp);
    printf("Running process: %d\n", runningState);
    printf("Ready queue: ");
    for (int i = 0; i < readyQueueSize; i++) printf("%d ", readyQueue[i]);
    printf("\nBlocked queue: ");
    for (int i = 0; i < blockedQueueSize; i++) printf("%d ", blockedQueue[i]);
    printf("\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PcbEntry *pcb = &pcbTable[i];
        if (pcb->state == STATE_READY || pcb->state == STATE_RUNNING || pcb->state == STATE_BLOCKED) {
            printf("PCB %d: PID=%d, ParentPID=%d, PC=%d, Value=%d, State=%s, Priority=%d, StartTime=%d, TimeUsed=%d\n",
                   i, pcb->processId, pcb->parentProcessId, pcb->programCounter, pcb->value,
                   (pcb->state == STATE_RUNNING ? "RUNNING" : (pcb->state == STATE_READY ? "READY" : "BLOCKED")),
                   pcb->priority, pcb->startTime, pcb->timeUsed);
        }
    }
}

int runProcessManager(int fileDescriptor, const char *filename) {
    if (!createProgram(filename, &pcbTable[0].program)) {
        return EXIT_FAILURE;
    }
    pcbTable[0].processId = 0;
    pcbTable[0].parentProcessId = -1;
    pcbTable[0].programCounter = 0;
    pcbTable[0].value = 0;
    pcbTable[0].priority = 0;
    pcbTable[0].state = STATE_RUNNING;
    pcbTable[0].startTime = 0;
    pcbTable[0].timeUsed = 0;
    runningState = 0;

    cpu.pProgram = &pcbTable[0].program;
    cpu.programCounter = pcbTable[0].programCounter;
    cpu.value = pcbTable[0].value;
    timestamp = 0;
    char ch;

    do {
        if (read(fileDescriptor, &ch, sizeof(ch)) != sizeof(ch)) {
            break;
        }
        switch (ch) {
            case 'Q':
                quantum();
                break;
            case 'U':
                unblock();
                break;
            case 'P':
                printState();
                break;
            default:
                printf("You entered an invalid character!\n");
        }
    } while (ch != 'T');
    printf("Average turnaround time: %.6f\n", numTerminatedProcesses ? cumulativeTimeDiff / numTerminatedProcesses : 0.0);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <program file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int pipeDescriptors[2];
    pid_t processMgrPid;
    char ch;
    int result;

    if (pipe(pipeDescriptors) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    if ((processMgrPid = fork()) == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (processMgrPid == 0) {
        close(pipeDescriptors[1]);
        result = runProcessManager(pipeDescriptors[0], argv[1]);
        close(pipeDescriptors[0]);
        _exit(result);
    } else {
        close(pipeDescriptors[0]);
        do {
            printf("Enter Q, P, U or T\n$ ");
            if (scanf(" %c", &ch) != 1) {
                fprintf(stderr, "Error reading input\n");
                break;
            }
            if (write(pipeDescriptors[1], &ch, sizeof(ch)) != sizeof(ch)) {
                perror("write");
                break;
            }
        } while (ch != 'T');
        if (write(pipeDescriptors[1], &ch, sizeof(ch)) != sizeof(ch)) {
            perror("write");
        }
        close(pipeDescriptors[1]);
        wait(&result);
    }
    return result;
}

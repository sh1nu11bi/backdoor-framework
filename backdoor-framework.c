/* A little client/server framework for writing back doors.
 *
 * Definitions:
 *   + backdoor: A backdoor is deliberate functionality that bypasses official publicly-documented authorization methods for
 *     that software and is intended by the author to be known to a limited audience.
 *
 *   + protected resource: some resource that should be accessed/modified only when the software has authorized an agent to
 *     do so.
 *
 *   + agent: user, process, object, etc. that can be authenticated and/or authorized to perform some action on a protected
 *     resource.
 *
 *   + authentication: confirmation that an agent is who it claims to be.
 *
 *   + authorization: confirmation that an agent is allowed to access some protected resource.
 *
 * Operation:
 *   This server represents firmware running on some hardware. Normal firmware is event driven by interrupts from hardware,
 *   such as sensors, and connections by agents such as via web, ssh, etc.  We want to keep things simple, so we treat agents
 *   and hardware interrupts the same way.  All agents and interrupts are represented by clients that connect to the server via
 *   Unix domain socket and issue zero or more commands to the server.  The server processes each command from each client
 *   sequentially and invokes the server_interrupt function synchronously after each command.
 *
 * Communication:
 *   Clients (agents, simulated hardware) connect to the server via Unix domain socket and send a one-byte command followed by
 *   some number of one-byte arguments.  The number of arguments is intrinsic to the command.  Each client may send zero or
 *   more commands, but the next client is not processed until the current one closes the connection.
 *
 * Usage:
 *   When invoked with no arguments, this program becomes the server.
 *
 *   When invoked with arguments, this program becomes the client. The arguments are the integer commands and command
 *   arguments. Command names and variable names are also accepted.
 *
 *   Example:  in one shell run "./backdoor-framework".  In another shell run these commands:
 *     $ ./backdoor-framework nop
 *     $ ./backdoor-framework set voltage 100
 *     $ ./backdoor-framework exit
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SERVER_ADDR "./backdoor-framework-socket"
#define MAX_BYTES_IN_COMMAND 3

/* Commands that can be executed by clients. Explicitly numbered so they're easy to identify when using the client. In order to
 * avoid confusion in shell scripts where clients are called with hard-coded numbers, please don't change these numbers once
 * the command is defined. */
enum ClientCommand {
    CMD_NOP                     = 0,                    /* no operation, but causes server_interrupt to execute */
    CMD_EXIT                    = 1,                    /* exit server without calling server_interrupt */
    CMD_SET_VARIABLE            = 2,                    /* set variable to value */
};

/* Variables used by the server and which can be set by clients.  Explicitly numbered so they're easy to use in shell scripts
 * that invoke the client.  Please don't change the numbers once you've defined a variable. */
enum VariableName {
    VAR_UNUSED                  = 0,                    /* just some default when the client didn't specify anything */
    VAR_VOLTAGE                 = 1,                    /* potential read from hardware */
    VAR_AMPERAGE                = 2,                    /* current read from hardware */
    VAR_MIN_VOLTAGE             = 3,                    /* min allowed voltage before circuit breaker trips */
    VAR_MAX_VOLTAGE             = 4,                    /* max allowed voltage before circuit breaker trips */
    VAR_CIRCUIT_BREAKER         = 5,                    /* circuit breaker state: 0 => open; non-zero => closed */
    VAR_LAST
};
static uint8_t vars[256] = {
    0,                                                  /* 0: unused */
    240,                                                /* 1: read voltage */
    0,                                                  /* 2: read current */
    235,                                                /* 3: min voltage */
    245,                                                /* 4: max voltage */
    1,                                                  /* 5: circuit breaker closed? */
};

static const char *
variable_name(enum VariableName name, int use_default) {
    static char dflt[64];
    switch (name) {
        case VAR_UNUSED:          return "unused";
        case VAR_VOLTAGE:         return "voltage";
        case VAR_AMPERAGE:        return "amperage";
        case VAR_MIN_VOLTAGE:     return "min_voltage";
        case VAR_MAX_VOLTAGE:     return "max_voltage";
        case VAR_CIRCUIT_BREAKER: return "circuit_breaker";
        default:
            if (!use_default)
                return NULL;
            sprintf(dflt, "var[%u]", (unsigned)name);
            return dflt;
    }
}

static void
show_variables(void) {
    int i;
    fputs("variables:\n", stdout);
    for (i=0; i<256; ++i) {
        if (variable_name(i, 0) || vars[i])
            printf("  %d: %-24s = %u\n", i, variable_name(i, 1), (unsigned)vars[i]);
    }
}

/* Example protected resource: tripping a circuit breaker. An agent is authenticated by virtue of having connected to the
 * server by the Unix domain socket. It is authorized to trip a cicuit breaker if voltage falls outside some range and the
 * breaker is not yet tripped. */
static void
trip_breaker_based_on_voltage(void) {
    int trip = vars[VAR_CIRCUIT_BREAKER]!=0 &&
               (vars[VAR_VOLTAGE] < vars[VAR_MIN_VOLTAGE] || vars[VAR_VOLTAGE] > vars[VAR_MAX_VOLTAGE]);
    if (trip) {
        vars[VAR_CIRCUIT_BREAKER] = 0;
        fputs("*** PROTECTED: circuit breaker tripped\n", stdout);
    }
}
    
/* Server internal processing loop.  In real hardware, this would be interrupt-based where all the real firmware work occurs,
 * but in this framework we just call it once per command received from a client. */
static void
server_interrupt(void) {
    fputs("server interrupt\n", stdout);
    trip_breaker_based_on_voltage();
    show_variables();
}

/* Set up the server's Unix-domain socket for listening or exit. */
static int
start_server_socket(void) {
    struct sockaddr_un server_addr;
    int sock = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (-1==sock) {
        perror("cannot open socket");
        exit(1);
    }
    server_addr.sun_family = AF_LOCAL;
    strncpy(server_addr.sun_path, SERVER_ADDR, sizeof(server_addr.sun_path));
    server_addr.sun_path[sizeof(server_addr.sun_path)-1] = '\0';
    if (-1==bind(sock, (struct sockaddr*)&server_addr, SUN_LEN(&server_addr))) {
        fprintf(stderr, "cannot bind socket %s: %s\n", server_addr.sun_path, strerror(errno));
        exit(1);
    }
    if (-1==listen(sock, 5)) {
        perror("cannot accept connections");
        exit(1);
    }
    printf("server is listening at %s\n", server_addr.sun_path);
    return sock;
}

/* Accept a connection from a client. */
static int
accept_client_connection(int server) {
    struct sockaddr_un client_addr;
    socklen_t client_addr_sz = 0;
    int client = accept(server, (struct sockaddr*)&client_addr, &client_addr_sz);
    if (-1 == client) {
        perror("accept");
        exit(1);
    }
    return client;
}

/* Process commands from a client. */
static void
process_client_commands(int client) {
    while (1) {
        unsigned char command = CMD_NOP;
        if (read(client, &command, 1) <= 0)
            return;
        switch (command) {
            case CMD_NOP:
                fputs("command: nop\n", stdout);
                break;
            case CMD_EXIT:
                fputs("command: exit\n", stdout);
                unlink(SERVER_ADDR);
                exit(0);
            case CMD_SET_VARIABLE: {
                unsigned char name_val[2];
                name_val[0] = VAR_UNUSED;
                name_val[1] = 0;
                (void) read(client, name_val, 2);
                printf("command: set variable[%u] = %u\n", (unsigned)name_val[0], (unsigned)name_val[1]);
                vars[name_val[0]] = name_val[1];
                break;
            }
            default:
                printf("unknown command: %u\n", (unsigned)command);
                break;
        }
        server_interrupt();
    }
}

/* Act as the server (i.e., pretend we're firmware) */
static void
server(void) {
    int client, server;
    server = start_server_socket();
    while ((client = accept_client_connection(server))) {
        process_client_commands(client);
        close(client);
    }
}

/* Act as the client (i.e., pretend we're an agent or hardware sensor) */
static void
client(int argc, char *argv[]) {
    uint8_t command[MAX_BYTES_IN_COMMAND];
    size_t command_size;
    int server, i;
    struct sockaddr_un server_addr;

    /* Check command-line */
    if (argc < 2) {
        fprintf(stderr, "usage: %s COMMAND [ARG...]\n"
                "  where COMMAND and ARG are non-negative integers in [0,255].\n",
                argv[0]);
        exit(1);
    }
    command_size = argc-1;
    if (command_size > MAX_BYTES_IN_COMMAND) {
        fputs("too many arguments\n", stderr);
        exit(1);
    }

    /* Obtain command from command-line */
    if (0==strcmp(argv[1], "nop")) {
        command[0] = CMD_NOP;
    } else if (0==strcmp(argv[1], "exit")) {
        command[0] = CMD_EXIT;
    } else if (0==strcmp(argv[1], "set")) {
        command[0] = CMD_SET_VARIABLE;
    } else {
        command[0] = strtoul(argv[1], NULL, 0);
    }

    /* Obtain command arguments from command-line */
    for (i=2; i<argc; ++i) {
        if (command[0]==CMD_SET_VARIABLE) {
            if (0==strcmp(argv[i], "voltage")) {
                command[i-1] = VAR_VOLTAGE;
            } else if (0==strcmp(argv[i], "amperage")) {
                command[i-1] = VAR_AMPERAGE;
            } else if (0==strcmp(argv[i], "min_voltage")) {
                command[i-1] = VAR_MIN_VOLTAGE;
            } else if (0==strcmp(argv[i], "max_voltage")) {
                command[i-1] = VAR_MAX_VOLTAGE;
            } else if (0==strcmp(argv[i], "circuit_breaker")) {
                command[i-1] = VAR_CIRCUIT_BREAKER;
            } else {
                command[i-1] = strtoul(argv[i], NULL, 0);
            }
        } else {
            command[i-1] = strtoul(argv[i], NULL, 0);
        }
    }

    /* Open connection to server */
    server = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (-1 == server) {
        perror("socket");
        exit(1);
    }
    server_addr.sun_family = AF_LOCAL;
    strncpy(server_addr.sun_path, SERVER_ADDR, sizeof(server_addr.sun_path));
    server_addr.sun_path[sizeof(server_addr.sun_path)-1] = '\0';
    if (-1==connect(server, (struct sockaddr*)&server_addr, SUN_LEN(&server_addr))) {
        fprintf(stderr, "cannot connect to server at %s: %s\n", server_addr.sun_path, strerror(errno));
        exit(1);
    }

    /* Send command */
    if (write(server, command, command_size)!=command_size)
        fputs("write failed or short write\n", stderr);
}

int
main(int argc, char *argv[]) {
    if (argc>1) {
        client(argc, argv);
    } else {
        server();
    }
    return 0;
}

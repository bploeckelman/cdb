#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>

// --------------------------------------

typedef struct {
    bool running;
} Application;
Application app = { .running = false };

// --------------------------------------


typedef struct {
    char *buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

typedef enum {
    EXECUTE_SUCCESS
    , EXECUTE_TABLE_FULL
} ExecuteResult;

typedef enum {
      META_COMMAND_SUCCESS
    , META_COMMAND_UNRECOGNIZED
} MetaCommandResult;

typedef enum {
      PREPARE_SUCCESS
    , PREPARE_SYNTAX_ERROR
    , PREPARE_NEGATIVE_ID
    , PREPARE_STRING_TOO_LONG
    , PREPARE_STATEMENT_UNRECOGNIZED
} PrepareResult;

typedef enum {
      STATEMENT_INSERT
    , STATEMENT_SELECT
} StatementType;

#define COLUMN_SIZE_USERNAME 32
#define COLUMN_SIZE_EMAIL 255
typedef struct {
    uint32_t id;
    char username[COLUMN_SIZE_USERNAME + 1];
    char email[COLUMN_SIZE_EMAIL + 1];
} Row;

typedef struct {
    StatementType type;
    Row row_to_insert; // only used by insert statement (union later?)
} Statement;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

const uint32_t  SIZE_ID       = size_of_attribute(Row, id);
const uint32_t  SIZE_USERNAME = size_of_attribute(Row, username);
const uint32_t  SIZE_EMAIL    = size_of_attribute(Row, email);

const uint32_t OFFSET_ID       = 0;
const uint32_t OFFSET_USERNAME = OFFSET_ID + SIZE_ID;
const uint32_t OFFSET_EMAIL    = OFFSET_USERNAME + SIZE_USERNAME;

const uint32_t  ROW_SIZE = SIZE_ID + SIZE_USERNAME + SIZE_EMAIL;

const uint32_t PAGE_SIZE = 4096;
#define TABLE_MAX_PAGES 100
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
const uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES;

typedef struct {
    int file_descriptor;
    uint32_t  file_length;
    void *pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
    Pager *pager;
    uint32_t num_rows;
} Table;

typedef struct {
    Table *table;
    uint32_t row_num;
    bool end_of_table; // Indicates a position one past the last element
} Cursor;

// --------------------------------------

void print_prompt();

// --------------------------------------

InputBuffer *new_input_buffer();
void read_input(InputBuffer *input_buffer);
void free_input_buffer(InputBuffer *input_buffer);

// --------------------------------------

MetaCommandResult handle_meta_command(InputBuffer *input_buffer, Table* table);

// --------------------------------------

PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement);
PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statement);

// --------------------------------------

void serialize_row(Row *source, void *dest) {
    memcpy (dest + OFFSET_ID,       &(source->id),    SIZE_ID);
    strncpy(dest + OFFSET_USERNAME, source->username, SIZE_USERNAME);
    strncpy(dest + OFFSET_EMAIL,    source->email,    SIZE_EMAIL);
}

void deserialize_row(void *source, Row *dest) {
    memcpy(&(dest->id),       source + OFFSET_ID,       SIZE_ID);
    memcpy(&(dest->username), source + OFFSET_USERNAME, SIZE_USERNAME);
    memcpy(&(dest->email),    source + OFFSET_EMAIL,    SIZE_EMAIL);
}

// --------------------------------------

void print_row();

Pager *pager_open(const char *filename);
void *get_page(Pager *pager, uint32_t page_num);
void pager_flush(Pager *pager, uint32_t page_num, uint32_t size);

Table *db_open(const char *filename);
void db_close(Table *table);

Cursor *table_start(Table *table);
Cursor *table_end(Table *table);
void *cursor_value(Cursor *cursor);
void cursor_advance(Cursor *cursor);

// --------------------------------------

ExecuteResult execute_statement(Statement *statement, Table *table);
ExecuteResult execute_select(Statement *statement, Table *table);
ExecuteResult execute_insert(Statement *statement, Table *table);

// --------------------------------------
// ----------------------------------------------------------------------------------
// --------------------------------------

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }
    const char *filename = argv[1];
    Table *table = db_open(filename);

    InputBuffer *input_buffer = new_input_buffer();
    app.running = true;
    while (app.running) {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->buffer[0] == '.') {
            switch (handle_meta_command(input_buffer, table)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED: {
                    printf("Unrecognized command '%s'.\n", input_buffer->buffer);
                    continue;
                }
            }
        }

        Statement statement = {};
        switch (prepare_statement(input_buffer, &statement)) {
            case PREPARE_SUCCESS:
                break;
            case PREPARE_NEGATIVE_ID: {
                printf("ID must be positive.\n");
                continue;
            }
            case PREPARE_STRING_TOO_LONG: {
                printf("String is too long.\n");
                continue;
            }
            case PREPARE_SYNTAX_ERROR: {
                printf("Syntax error. Could not parse statement.\n");
                continue;
            }
            case PREPARE_STATEMENT_UNRECOGNIZED: {
                printf("Unrecognized keyword at start of '%s'\n", input_buffer->buffer);
                continue;
            }
        }

        switch (execute_statement(&statement, table)) {
            case EXECUTE_SUCCESS: {
                printf("Executed.\n");
            } break;
            case EXECUTE_TABLE_FULL: {
                printf("Error: Table full.\n");
            } break;
        }
    }

    free_input_buffer(input_buffer);

    return EXIT_SUCCESS;
}

// --------------------------------------
// ----------------------------------------------------------------------------------
// --------------------------------------

void print_prompt() {
    printf("db > ");
}

// --------------------------------------

InputBuffer *new_input_buffer() {
    InputBuffer *input_buffer = calloc(1, sizeof(InputBuffer));
    *input_buffer = (InputBuffer) {
            .buffer = NULL,
            .buffer_length = 0,
            .input_length = 0
    };
    return input_buffer;
}

void read_input(InputBuffer *input_buffer) {
    ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);
    if (bytes_read <= 0) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }

    // Ignore trailing newline
    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
}

void free_input_buffer(InputBuffer *input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

MetaCommandResult handle_meta_command(InputBuffer *input_buffer, Table *table) {
    if (0 == strcmp(input_buffer->buffer, ".exit")) {
        app.running = false;
        db_close(table);
        return META_COMMAND_SUCCESS;
    } else {
        return META_COMMAND_UNRECOGNIZED;
    }
}

PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement) {
    if (0 == strncmp(input_buffer->buffer, "insert", 6)) {
        return prepare_insert(input_buffer, statement);
    }
    if (0 == strcmp(input_buffer->buffer, "select")) {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }

    return PREPARE_STATEMENT_UNRECOGNIZED;
}

PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statement) {
    statement->type = STATEMENT_INSERT;

    char *keyword   = strtok(input_buffer->buffer, " ");
    char *id_string = strtok(NULL, " ");
    char *username  = strtok(NULL, " ");
    char *email     = strtok(NULL, " ");

    if (id_string == NULL || username == NULL || email == NULL) {
        return PREPARE_SYNTAX_ERROR;
    }

    int id = atoi(id_string);
    if (id < 0) {
        return PREPARE_NEGATIVE_ID;
    }
    if (strlen(username) > COLUMN_SIZE_USERNAME) {
        return PREPARE_STRING_TOO_LONG;
    }
    if (strlen(email) > COLUMN_SIZE_EMAIL) {
        return PREPARE_STRING_TOO_LONG;
    }

    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username, username);
    strcpy(statement->row_to_insert.email,    email);

    return PREPARE_SUCCESS;
}

void print_row(Row *row) {
    printf("[%d, %s, %s]\n", row->id, row->username, row->email);
}

void *get_page(Pager *pager, uint32_t page_num) {
    if (page_num > TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %d > %d\n", page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        // Cache miss. Allocate memory and load from file.
        void *page = malloc(PAGE_SIZE);
        uint32_t num_pages = pager->file_length / PAGE_SIZE;

        // We might save a partial page at the end of the file
        if (pager->file_length % PAGE_SIZE) {
            num_pages++;
        }

        if (page_num <= num_pages) {
            lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if (bytes_read == -1) {
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }

        pager->pages[page_num] = page;
    }

    return pager->pages[page_num];
}

Pager *pager_open(const char *filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);

    Pager *pager = calloc(1, sizeof(Pager));
    *pager = (Pager) {
        .file_descriptor = fd,
        .file_length = file_length
    };

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; ++i) {
        pager->pages[i] = NULL;
    }

    return pager;
}

void pager_flush(Pager *pager, uint32_t page_num, uint32_t size) {
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
    if (offset == -1) {
       printf("Error seeking: %d\n", errno);
       exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(pager->file_descriptor, pager->pages[page_num], size);
    if (bytes_written == -1) {
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

Table *db_open(const char *filename) {
    Pager *pager = pager_open(filename);
    uint32_t num_rows = pager->file_length / ROW_SIZE;

    Table *table = calloc(1, sizeof(Table));
    *table = (Table) {
        .pager = pager,
        .num_rows = num_rows
    };
    return table;
}

void db_close(Table *table) {
    Pager *pager = table->pager;
    uint32_t num_full_pages = table->num_rows / ROWS_PER_PAGE;

    for (uint32_t i = 0; i < num_full_pages; ++i) {
        if (pager->pages[i] == NULL) {
            continue;
        }
        pager_flush(pager, i , PAGE_SIZE);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

    // There may be a partial page to write to the end of the file
    // This should not be needed after we switch to a B-tree
    uint32_t  num_additional_rows = table->num_rows % ROWS_PER_PAGE;
    if (num_additional_rows > 0) {
        uint32_t page_num = num_full_pages;
        if (pager->pages[page_num] != NULL) {
            pager_flush(pager, page_num, num_additional_rows * ROW_SIZE);
            free(pager->pages[page_num]);
            pager->pages[page_num] = NULL;
        }
    }

    int result = close(pager->file_descriptor);
    if (result == -1) {
        printf("Error closing db file.\n");
        exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; ++i) {
        void *page = pager->pages[i];
        if (page) {
            free(page);
            pager->pages[i] = NULL;
        }
    }
    free(pager);
    free(table);
}

Cursor *table_start(Table *table) {
    Cursor *cursor = calloc(1, sizeof(Cursor));
    *cursor = (Cursor) {
        .table = table,
        .row_num = 0,
        .end_of_table = (table->num_rows == 0)
    };
    return cursor;
}

Cursor *table_end(Table *table) {
    Cursor *cursor = calloc(1, sizeof(Cursor));
    *cursor = (Cursor) {
            .table = table,
            .row_num = table->num_rows,
            .end_of_table = true
    };
    return cursor;
}

void *cursor_value(Cursor *cursor) {
    uint32_t row_num  = cursor->row_num;
    uint32_t page_num = row_num / ROWS_PER_PAGE;

    void *page = get_page(cursor->table->pager, page_num);

    uint32_t row_offset = row_num % ROWS_PER_PAGE;
    uint32_t byte_offset = row_offset * ROW_SIZE;

    return page + byte_offset;
}

void cursor_advance(Cursor *cursor) {
    cursor->row_num++;
    if (cursor->row_num >= cursor->table->num_rows) {
        cursor->end_of_table = true;
    }
}

ExecuteResult execute_insert(Statement *statement, Table *table) {
    if (table->num_rows >= TABLE_MAX_ROWS) {
        return EXECUTE_TABLE_FULL;
    }

    Cursor *cursor = table_end(table);
    Row *row_to_insert = &(statement->row_to_insert);

    serialize_row(row_to_insert, cursor_value(cursor));
    table->num_rows++;

    free(cursor);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement *statement, Table *table) {
    Cursor *cursor = table_start(table);

    Row row;
    while (!(cursor->end_of_table)) {
        deserialize_row(cursor_value(cursor), &row);
        print_row(&row);
        cursor_advance(cursor);
    }

    free(cursor);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement *statement, Table *table) {
    switch (statement->type) {
        case STATEMENT_INSERT: {
            return execute_insert(statement, table);
        }
        case STATEMENT_SELECT: {
            return execute_select(statement, table);
        }
    }
}

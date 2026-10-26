; The Terrarium assembler, written in Terrarium assembly.
;
;     asm -o program.bin [-s program.sym] file.asm...
;
; It makes two passes over the source. The first finds the address of every
; label, the second encodes the instructions. The language is described in
; spec/assembly.md.
;
; Calls pass arguments in r0-r3 and return results in r0 (and r1). A call may
; change r0-r3; r4-r7 survive every call.

SYS_EXIT = 0
SYS_READ = 1
SYS_WRITE = 2
SYS_OPEN = 3
SYS_CLOSE = 4
SYS_ARGS = 5
SYS_ARG = 6

STDERR = 2
GUARD = 0x10000
HEADER_SIZE = 32
DEFAULT_STACK = 0x10000

SOURCE_SIZE = 0x800000
CODE_SIZE = 0x400000
DATA_SIZE = 0x200000
SYMBOL_MASK = 0xffff            ; the table has 0x10000 entries
SYMBOL_SIZE = 20
MAX_SYMBOLS = 0xc000
TOKEN_SIZE = 12
MAX_FILES = 64
FILE_SIZE = 16
ARGS_SIZE = 0x4000

; Token kinds. A token is three words: kind, a, b.
T_END = 0
T_NAME = 1                      ; a = text, b = length
T_REG = 2                       ; a = register number
T_NUM = 3                       ; a = value
T_STR = 4                       ; a = decoded bytes, b = length
T_OP = 5                        ; a = operator id (its index in operators)
T_DIR = 6                       ; a = directive id
T_KEY = 7                       ; a = keyword id

K_IF = 0
K_GOTO = 1
K_CALL = 2
K_RETURN = 3
K_PUSH = 4
K_POP = 5
K_COPY = 6
K_FILL = 7
K_SYS = 8
K_HALT = 9
K_IO = 10
K_RESUME = 11
K_WAIT = 12
K_U8 = 13
K_U16 = 14
K_U32 = 15

D_CODE = 0
D_DATA = 1
D_BSS = 2
D_BYTE = 3
D_WORD = 4
D_ZERO = 5
D_ALIGN = 6
D_MEMORY = 7
D_STACK = 8
D_ALLOW = 9

OP_PLUS = 30
OP_MINUS = 31
OP_ASSIGN = 40
OP_LBRACKET = 41
OP_RBRACKET = 42
OP_COMMA = 43
OP_COLON = 44

; A symbol is five words: name, name length, kind, value, line.
S_CONST = 1
S_CODE = 2                      ; labels: S_CODE plus the section number
S_DATA = 3
S_BSS = 4

; ---------------------------------------------------------------------------
; Main

start:
    call read_arguments
    call read_sources
    r0 = 1
    call assemble
    r0 = offsets
    r0 = [r0 + 4]               ; data size
    r0 += 3
    r0 >>= 2
    r0 <<= 2
    r1 = GUARD
    r0 += r1
    r1 = bss_base
    [r1] = r0
    r0 = 2
    call assemble
    call write_executable
    call write_symbols
    r0 = 0
    sys SYS_EXIT

; Reads "-o path", "-s path" and the input files from the arguments.
read_arguments:
    push r4, r5, r6, r7
    sys SYS_ARGS
    r4 = r0                     ; argument count
    r5 = 0                      ; argument index
    r6 = argument_text          ; where the next argument goes
    r7 = 0                      ; 1 after "-o", 2 after "-s"
read_arguments_next:
    if r5 == r4 goto read_arguments_done
    r0 = r5
    r1 = r6
    r2 = argument_text_end
    r2 -= r6
    sys SYS_ARG
    if r0 > r2 goto read_arguments_long
    r1 = 1
    if r7 == r1 goto read_arguments_output
    r1 = 2
    if r7 == r1 goto read_arguments_symbols
    r1 = r6
    r2 = r0
    push r0
    r0 = option_o
    r3 = 2
    call text_equal
    r1 = r0
    pop r0
    r7 = 1
    if r1 != 0 goto read_arguments_skip
    r1 = r6
    r2 = r0
    push r0
    r0 = option_s
    r3 = 2
    call text_equal
    r1 = r0
    pop r0
    r7 = 2
    if r1 != 0 goto read_arguments_skip
    r7 = 0
    r1 = file_count
    r2 = [r1]
    r3 = MAX_FILES
    if r2 >= r3 goto read_arguments_files
    r3 = r2 + 1
    [r1] = r3
    r2 *= FILE_SIZE
    r1 = files
    r1 += r2
    [r1] = r6
    [r1 + 4] = r0
    goto read_arguments_skip
read_arguments_output:
    r1 = output_name
    [r1] = r6
    [r1 + 4] = r0
    r7 = 0
    goto read_arguments_skip
read_arguments_symbols:
    r1 = symbols_name
    [r1] = r6
    [r1 + 4] = r0
    r7 = 0
read_arguments_skip:
    r6 += r0
    r5 += 1
    goto read_arguments_next
read_arguments_done:
    if r7 != 0 goto read_arguments_usage
    r0 = output_name
    r0 = [r0]
    if r0 == 0 goto read_arguments_usage
    r0 = file_count
    r0 = [r0]
    if r0 == 0 goto read_arguments_usage
    pop r7, r6, r5, r4
    return
read_arguments_usage:
    r0 = message_usage
    call fatal
read_arguments_long:
    r0 = message_long_arguments
    call fatal
read_arguments_files:
    r0 = message_too_many_files
    call fatal

; Returns r0 = 1 if the text at r0 (length r3) equals the text at r1 (length r2).
text_equal:
    if r2 != r3 goto text_equal_no
    call bytes_equal
    return
text_equal_no:
    r0 = 0
    return

; Reads every input file into the source buffer.
read_sources:
    push r4, r5, r6, r7
    r4 = files
    r5 = file_count
    r5 = [r5]
    r5 *= FILE_SIZE
    r5 += r4                    ; end of the file table
    r6 = source                 ; where the next file goes
read_sources_file:
    if r4 == r5 goto read_sources_done
    [r4 + 8] = r6
    r0 = [r4]
    r1 = [r4 + 4]
    r2 = 0
    sys SYS_OPEN
    r1 = -1
    if r0 == r1 goto read_sources_missing
    r7 = r0                     ; file
read_sources_read:
    r0 = r7
    r1 = r6
    r2 = source_end
    r2 -= r6
    if r2 == 0 goto read_sources_full
    sys SYS_READ
    r1 = -1
    if r0 == r1 goto read_sources_missing
    if r0 == 0 goto read_sources_eof
    r6 += r0
    goto read_sources_read
read_sources_eof:
    r0 = r7
    sys SYS_CLOSE
    [r4 + 12] = r6
    r4 += FILE_SIZE
    goto read_sources_file
read_sources_done:
    pop r7, r6, r5, r4
    return
read_sources_missing:
    r0 = message_cant_read
    r1 = [r4]
    r2 = [r4 + 4]
    call fatal_name
read_sources_full:
    r0 = message_too_much_source
    call fatal

; ---------------------------------------------------------------------------
; Passes

; Runs pass r0 (1 or 2) over every file.
assemble:
    push r4, r5, r6, r7
    r1 = pass
    [r1] = r0
    r0 = 0
    r1 = section
    [r1] = r0
    r1 = offsets
    [r1] = r0
    [r1 + 4] = r0
    [r1 + 8] = r0
    r1 = line
    [r1] = r0
    r1 = memory_set
    [r1] = r0
    r1 = stack_set
    [r1] = r0
    r1 = allow_set
    [r1] = r0
    r4 = files
    r5 = file_count
    r5 = [r5]
    r5 *= FILE_SIZE
    r5 += r4
assemble_file:
    if r4 == r5 goto assemble_done
    r0 = current_file
    [r0] = r4
    r0 = 0
    r1 = file_line
    [r1] = r0
    r6 = [r4 + 8]               ; position
    r7 = [r4 + 12]              ; end
assemble_line:
    if r6 >= r7 goto assemble_next_file
    r0 = file_line
    r1 = [r0]
    r1 += 1
    [r0] = r1
    r0 = line
    r1 = [r0]
    r1 += 1
    [r0] = r1
    r0 = r6
    r1 = r7
    call tokenize
    r6 = r0
    call statement
    goto assemble_line
assemble_next_file:
    r4 += FILE_SIZE
    goto assemble_file
assemble_done:
    pop r7, r6, r5, r4
    return

; ---------------------------------------------------------------------------
; Tokens

; Splits the line at r0 into tokens, stopping at r1 (the end of the file).
; Returns r0 = the start of the next line.
tokenize:
    push r4, r5, r6, r7
    r4 = r0                     ; position
    r5 = r1                     ; end of the file
    r6 = tokens                 ; next token
    r7 = strings                ; next byte of decoded strings
    r0 = cursor
    [r0] = r6
tokenize_next:
    if r4 >= r5 goto tokenize_done
    r0 = u8 [r4]
    r1 = '\n'
    if r0 == r1 goto tokenize_newline
    r1 = ' '
    if r0 == r1 goto tokenize_space
    r1 = '\t'
    if r0 == r1 goto tokenize_space
    r1 = '\r'
    if r0 == r1 goto tokenize_space
    r1 = ';'
    if r0 == r1 goto tokenize_comment
    r1 = tokens_last
    if r6 >= r1 goto tokenize_long
    r1 = '.'
    if r0 == r1 goto tokenize_dot
    r1 = '\''
    if r0 == r1 goto tokenize_char
    r1 = '"'
    if r0 == r1 goto tokenize_string
    r1 = char_classes
    r1 += r0
    r1 = u8 [r1]
    r2 = 1
    if r1 == r2 goto tokenize_number
    r2 = 2
    if r1 == r2 goto tokenize_word
    goto tokenize_operator

tokenize_space:
    r4 += 1
    goto tokenize_next

tokenize_comment:
    if r4 >= r5 goto tokenize_done
    r0 = u8 [r4]
    r1 = '\n'
    if r0 == r1 goto tokenize_newline
    r4 += 1
    goto tokenize_comment

tokenize_newline:
    r4 += 1
tokenize_done:
    r0 = T_END
    [r6] = r0
    r0 = r4
    pop r7, r6, r5, r4
    return

tokenize_dot:
    r0 = r4 + 1
    if r0 >= r5 goto tokenize_operator
    r0 = u8 [r0]
    r1 = char_classes
    r1 += r0
    r1 = u8 [r1]
    r2 = 2
    if r1 != r2 goto tokenize_operator
    r0 = r4 + 1
    r1 = r5
    call scan_word
    r1 = r0
    r0 = r4 + 1                 ; directive name without the dot
    r4 = r1
    r1 -= r0
    r2 = directives
    call find_word
    r1 = -1
    if r0 == r1 goto tokenize_bad_directive
    r1 = T_DIR
    goto tokenize_store

tokenize_word:
    r0 = r4
    r1 = r5
    call scan_word
    r1 = r0
    r0 = r4                     ; start
    r4 = r1                     ; continue after the word
    r1 -= r0                    ; length
    push r0, r1
    call register_number
    r2 = r0
    pop r1, r0
    r3 = -1
    if r2 == r3 goto tokenize_keyword
    r0 = r2
    r1 = T_REG
    goto tokenize_store
tokenize_keyword:
    push r0, r1
    r2 = keywords
    call find_word
    r2 = r0
    pop r1, r0
    r3 = -1
    if r2 == r3 goto tokenize_name
    r0 = r2
    r1 = T_KEY
    goto tokenize_store
tokenize_name:
    [r6 + 4] = r0
    [r6 + 8] = r1
    r0 = T_NAME
    [r6] = r0
    r6 += TOKEN_SIZE
    goto tokenize_next

tokenize_number:
    r0 = r4
    r1 = r5
    call scan_word
    r1 = r0
    r0 = r4
    r4 = r1
    call parse_number
    r1 = T_NUM
    goto tokenize_store

tokenize_char:
    r0 = r4 + 1
    r1 = r5
    call read_char
    r4 = r1
    if r4 >= r5 goto tokenize_unterminated_char
    r1 = u8 [r4]
    r2 = '\''
    if r1 != r2 goto tokenize_unterminated_char
    r4 += 1
    r1 = T_NUM
    goto tokenize_store

tokenize_string:
    r4 += 1
    [r6 + 4] = r7
tokenize_string_next:
    if r4 >= r5 goto tokenize_unterminated_string
    r0 = u8 [r4]
    r1 = '"'
    if r0 == r1 goto tokenize_string_end
    r0 = r4
    r1 = r5
    call read_char
    r4 = r1
    r1 = strings_end
    if r7 >= r1 goto tokenize_long
    u8 [r7] = r0
    r7 += 1
    goto tokenize_string_next
tokenize_string_end:
    r4 += 1
    r0 = [r6 + 4]
    r0 = r7 - r0
    [r6 + 8] = r0
    r0 = T_STR
    [r6] = r0
    r6 += TOKEN_SIZE
    goto tokenize_next

tokenize_operator:
    r0 = r4
    r1 = r5
    call match_operator
    r2 = -1
    if r0 == r2 goto tokenize_unexpected
    r4 += r1
    r1 = T_OP

; Stores a token of kind r1 with value r0.
tokenize_store:
    [r6] = r1
    [r6 + 4] = r0
    r6 += TOKEN_SIZE
    goto tokenize_next

tokenize_long:
    r0 = message_long_line
    call fail
tokenize_bad_directive:
    r0 = message_unknown_directive
    call fail
tokenize_unterminated_char:
    r0 = message_unterminated_char
    call fail
tokenize_unterminated_string:
    r0 = message_unterminated_string
    call fail
tokenize_unexpected:
    r0 = message_unexpected_character
    call fail

; Returns r0 = the end of the name or number starting at r0, stopping at r1.
scan_word:
    if r0 >= r1 goto scan_word_done
    r2 = u8 [r0]
    r3 = char_classes
    r3 += r2
    r3 = u8 [r3]
    if r3 == 0 goto scan_word_done
    r0 += 1
    goto scan_word
scan_word_done:
    return

; Returns the register number of the name at r0 (length r1), or -1.
register_number:
    r2 = 2
    if r1 != r2 goto register_number_none
    r2 = u8 [r0]
    r1 = u8 [r0 + 1]
    r3 = 's'
    if r2 != r3 goto register_number_r
    r3 = 'p'
    if r1 != r3 goto register_number_none
    r0 = 8
    return
register_number_r:
    r3 = 'r'
    if r2 != r3 goto register_number_none
    r1 -= '0'
    r3 = 8
    if r1 >= r3 goto register_number_none
    r0 = r1
    return
register_number_none:
    r0 = -1
    return

; Parses the number from r0 to r1. Returns r0 = its value.
parse_number:
    push r4, r5, r6, r7
    r4 = r0                     ; position
    r5 = r1                     ; end
    r6 = 0                      ; value
    r7 = 10                     ; base
    r0 = r5 - r4
    r1 = 2
    if r0 < r1 goto parse_number_digits
    r0 = u8 [r4]
    r1 = '0'
    if r0 != r1 goto parse_number_digits
    r0 = u8 [r4 + 1]
    r0 |= 0x20
    r1 = 'x'
    if r0 != r1 goto parse_number_bad
    r4 += 2
    r7 = 16
    if r4 == r5 goto parse_number_bad
parse_number_digits:
    if r4 == r5 goto parse_number_done
    r0 = u8 [r4]
    r1 = digit_values
    r1 += r0
    r0 = u8 [r1]
    if r0 >= r7 goto parse_number_bad
    r1 = -1                     ; the largest value that can still take this digit
    r1 -= r0
    r1 /= r7
    if r6 > r1 goto parse_number_bad
    r6 *= r7
    r6 += r0
    r4 += 1
    goto parse_number_digits
parse_number_done:
    r0 = r6
    pop r7, r6, r5, r4
    return
parse_number_bad:
    r0 = message_bad_number
    call fail

; Reads one character or escape at r0, stopping at r1.
; Returns r0 = its value and r1 = the position after it.
read_char:
    if r0 >= r1 goto read_char_unterminated
    r2 = u8 [r0]
    r3 = '\n'
    if r2 == r3 goto read_char_unterminated
    r3 = '\\'
    if r2 == r3 goto read_char_escape
    r1 = r0 + 1
    r0 = r2
    return
read_char_escape:
    push r4
    r0 += 1
    if r0 >= r1 goto read_char_bad_escape
    r2 = u8 [r0]
    r3 = escapes
read_char_escape_next:
    r4 = u8 [r3]
    if r4 == 0 goto read_char_bad_escape
    if r4 == r2 goto read_char_escape_found
    r3 += 2
    goto read_char_escape_next
read_char_escape_found:
    r1 = r0 + 1
    r0 = u8 [r3 + 1]
    pop r4
    return
read_char_unterminated:
    r0 = message_unterminated_char
    call fail
read_char_bad_escape:
    r0 = message_bad_escape
    call fail

; Finds the longest operator at r0, stopping at r1.
; Returns r0 = its id (or -1) and r1 = its length.
match_operator:
    push r4, r5, r6, r7
    r4 = r0                     ; position
    r5 = r1                     ; end
    r6 = operators              ; table entry
    r7 = 0                      ; id
match_operator_next:
    r3 = u8 [r6]
    if r3 == 0 goto match_operator_none
    r0 = r4 + r3
    if r0 > r5 goto match_operator_skip
    r0 = r4
    r1 = r6 + 1
    r2 = r3
    call bytes_equal
    if r0 == 0 goto match_operator_skip
    ; An operator ending in a letter can't run into a name: "/size" is "/"
    ; then "size".
    r3 = u8 [r6]
    r0 = r6 + r3
    r0 = u8 [r0]
    r1 = char_classes
    r1 += r0
    r1 = u8 [r1]
    r2 = 2
    if r1 != r2 goto match_operator_found
    r0 = r4 + r3
    if r0 >= r5 goto match_operator_found
    r0 = u8 [r0]
    r1 = char_classes
    r1 += r0
    r1 = u8 [r1]
    if r1 == 0 goto match_operator_found
match_operator_skip:
    r3 = u8 [r6]
    r6 += r3
    r6 += 1
    r7 += 1
    goto match_operator_next
match_operator_found:
    r0 = r7
    r1 = u8 [r6]
    pop r7, r6, r5, r4
    return
match_operator_none:
    r0 = -1
    pop r7, r6, r5, r4
    return

; Finds the word at r0 (length r1) in the table at r2, a list of
; length-prefixed words ending with 0. Returns r0 = its index, or -1.
find_word:
    push r4, r5, r6, r7
    r4 = r0
    r5 = r1
    r6 = r2
    r7 = 0
find_word_next:
    r3 = u8 [r6]
    if r3 == 0 goto find_word_none
    if r3 != r5 goto find_word_skip
    r0 = r4
    r1 = r6 + 1
    r2 = r5
    call bytes_equal
    if r0 != 0 goto find_word_found
find_word_skip:
    r3 = u8 [r6]
    r6 += r3
    r6 += 1
    r7 += 1
    goto find_word_next
find_word_found:
    r0 = r7
    pop r7, r6, r5, r4
    return
find_word_none:
    r0 = -1
    pop r7, r6, r5, r4
    return

; Returns r0 = 1 if the r2 bytes at r0 and r1 are equal.
bytes_equal:
    push r4
bytes_equal_next:
    if r2 == 0 goto bytes_equal_yes
    r3 = u8 [r0]
    r4 = u8 [r1]
    if r3 != r4 goto bytes_equal_no
    r0 += 1
    r1 += 1
    r2 -= 1
    goto bytes_equal_next
bytes_equal_yes:
    r0 = 1
    pop r4
    return
bytes_equal_no:
    r0 = 0
    pop r4
    return

; ---------------------------------------------------------------------------
; Reading tokens

; Returns the current token: r0 = kind, r1 = a, r2 = b. Changes only r0-r2.
peek:
    r2 = cursor
    r2 = [r2]
    r0 = [r2]
    r1 = [r2 + 4]
    r2 = [r2 + 8]
    return

; Moves to the next token. Changes only r0-r1.
advance:
    r1 = cursor
    r0 = [r1]
    r0 += TOKEN_SIZE
    [r1] = r0
    return

; If the current token is operator r0, moves past it and returns r0 = 1.
take_op:
    r3 = r0
    call peek
    r2 = T_OP
    if r0 != r2 goto take_op_no
    if r1 != r3 goto take_op_no
    call advance
    r0 = 1
    return
take_op_no:
    r0 = 0
    return

; Moves past operator r0, or fails.
expect_op:
    push r4
    r4 = r0
    call take_op
    if r0 == 0 goto expect_op_fail
    pop r4
    return
expect_op_fail:
    r0 = message_expected
    r1 = operators
    r1 += 1
expect_op_find:
    if r4 == 0 goto expect_op_found
    r2 = u8 [r1 - 1]
    r1 += r2
    r1 += 1
    r4 -= 1
    goto expect_op_find
expect_op_found:
    r2 = u8 [r1 - 1]
    call fail_name

; Returns r0 = the number of the register at the cursor, or fails.
expect_reg:
    call peek
    r2 = T_REG
    if r0 != r2 goto expect_reg_fail
    push r1
    call advance
    pop r0
    return
expect_reg_fail:
    r0 = message_expected_register
    call fail

; Fails unless the cursor is at the end of the line.
expect_end:
    call peek
    if r0 != 0 goto expect_end_fail
    return
expect_end_fail:
    r0 = message_trailing_text
    call fail

; If the current token is keyword r0, moves past it and returns r0 = 1.
take_keyword:
    r3 = r0
    call peek
    r2 = T_KEY
    if r0 != r2 goto take_op_no
    if r1 != r3 goto take_op_no
    call advance
    r0 = 1
    return

; Reads an optional size prefix (u8, u16, u32).
; Returns r0 = 0 for 32 bits, 1 for 16, 2 for 8, and r1 = 1 if there was one.
size_prefix:
    call peek
    r2 = T_KEY
    if r0 != r2 goto size_prefix_none
    r2 = K_U32
    if r1 == r2 goto size_prefix_32
    r2 = K_U16
    if r1 == r2 goto size_prefix_16
    r2 = K_U8
    if r1 != r2 goto size_prefix_none
    call advance
    r0 = 2
    r1 = 1
    return
size_prefix_16:
    call advance
    r0 = 1
    r1 = 1
    return
size_prefix_32:
    call advance
    r0 = 0
    r1 = 1
    return
size_prefix_none:
    r0 = 0
    r1 = 0
    return

; ---------------------------------------------------------------------------
; Values

; Reads an optional '-' and a number or name.
; Returns r0 = the value, r2 = the name's symbol (0 for numbers and unknown
; names), and r1 = 0 for a number, 1 for a name with a known value, or 2 for
; a name whose value isn't known yet (in pass 1).
atom:
    push r4, r5, r6
    r0 = OP_MINUS
    call take_op
    r4 = r0                     ; negate?
    call peek
    r3 = T_NUM
    if r0 == r3 goto atom_number
    r3 = T_NAME
    if r0 != r3 goto atom_bad
    r5 = r1
    r6 = r2
    call advance
    r0 = r5
    r1 = r6
    call find_symbol
    if r1 == 0 goto atom_undefined
    r1 = [r0 + 8]
    r2 = S_CONST
    if r1 != r2 goto atom_label
    r1 = [r0 + 16]
    r2 = line
    r2 = [r2]
    if r1 >= r2 goto atom_early
    r2 = r0
    r0 = [r0 + 12]
    r1 = 1
    goto atom_sign
atom_label:
    if r4 != 0 goto atom_negated_label
    r5 = r0
    r1 = pass
    r1 = [r1]
    r2 = 1
    if r1 == r2 goto atom_unknown
    call label_address
    r2 = r5
    r1 = 1
    goto atom_sign
atom_undefined:
    r1 = pass
    r1 = [r1]
    r2 = 1
    if r1 != r2 goto atom_missing
atom_unknown:
    r0 = 0
    r1 = 2
    r2 = 0
    pop r6, r5, r4
    return
atom_number:
    r5 = r1
    call advance
    r0 = r5
    r1 = 0
    r2 = 0
atom_sign:
    if r4 == 0 goto atom_done
    r0 ^= -1
    r0 += 1
atom_done:
    pop r6, r5, r4
    return
atom_bad:
    r0 = message_expected_value
    call fail
atom_missing:
    r0 = message_undefined
    r1 = r5
    r2 = r6
    call fail_name
atom_early:
    r0 = message_early_constant
    r1 = r5
    r2 = r6
    call fail_name
atom_negated_label:
    r0 = message_negated_label
    call fail

; Reads a number or constant. Returns r0 = its value.
constant:
    call atom
    if r1 == 0 goto constant_done
    if r2 == 0 goto constant_bad
    r3 = [r2 + 8]
    r1 = S_CONST
    if r3 != r1 goto constant_bad
constant_done:
    return
constant_bad:
    r0 = message_expected_constant
    call fail

; Reads a constant that fits in 16 signed bits. Returns r0 = its low 16 bits.
immediate:
    call constant
    r1 = r0 + 0x7fff
    r1 += 1
    r2 = 0xffff
    if r1 > r2 goto immediate_bad
    r0 <<= 16
    r0 >>= 16
    return
immediate_bad:
    r0 = message_too_big
    call fail

; Reads a label in .code. Returns r0 = its address (0 in pass 1).
code_label:
    push r4, r5
    call peek
    r3 = T_NAME
    if r0 != r3 goto code_label_bad
    r4 = r1
    r5 = r2
    call advance
    r0 = pass
    r0 = [r0]
    r1 = 1
    if r0 == r1 goto code_label_pass1
    r0 = r4
    r1 = r5
    call find_symbol
    if r1 == 0 goto code_label_missing
    r1 = [r0 + 8]
    r2 = S_CODE
    if r1 != r2 goto code_label_wrong
    call label_address
    pop r5, r4
    return
code_label_pass1:
    r0 = 0
    pop r5, r4
    return
code_label_bad:
    r0 = message_expected_label
    call fail
code_label_missing:
    r0 = message_undefined
    r1 = r4
    r2 = r5
    call fail_name
code_label_wrong:
    r0 = message_not_code_label
    r1 = r4
    r2 = r5
    call fail_name

; ---------------------------------------------------------------------------
; Symbols

; Looks up the name at r0 (length r1). Returns r0 = its symbol, or the empty
; slot where it belongs, and r1 = 1 if it was found.
find_symbol:
    push r4, r5, r6, r7
    r4 = r0
    r5 = r1
    r6 = 0x811c9dc5             ; FNV-1a hash
    r2 = 0
find_symbol_hash:
    if r2 == r5 goto find_symbol_probe
    r3 = r4 + r2
    r3 = u8 [r3]
    r6 ^= r3
    r3 = 0x01000193
    r6 *= r3
    r2 += 1
    goto find_symbol_hash
find_symbol_probe:
    r3 = SYMBOL_MASK
    r6 &= r3
    r7 = r6 * SYMBOL_SIZE
    r0 = symbols
    r7 += r0
    r0 = [r7]
    if r0 == 0 goto find_symbol_empty
    r1 = [r7 + 4]
    if r1 != r5 goto find_symbol_skip
    r1 = r4
    r2 = r5
    call bytes_equal
    if r0 != 0 goto find_symbol_found
find_symbol_skip:
    r6 += 1
    goto find_symbol_probe
find_symbol_found:
    r0 = r7
    r1 = 1
    pop r7, r6, r5, r4
    return
find_symbol_empty:
    r0 = r7
    r1 = 0
    pop r7, r6, r5, r4
    return

; Defines the name at r0 (length r1) as kind r2 with value r3.
define:
    push r4, r5, r6, r7
    r4 = r0
    r5 = r1
    r6 = r2
    r7 = r3
    call find_symbol
    r2 = pass
    r2 = [r2]
    r3 = 1
    if r2 != r3 goto define_check
    if r1 != 0 goto define_twice
    [r0] = r4
    [r0 + 4] = r5
    [r0 + 8] = r6
    [r0 + 12] = r7
    r1 = line
    r1 = [r1]
    [r0 + 16] = r1
    r0 = symbol_count
    r1 = [r0]
    r1 += 1
    [r0] = r1
    r2 = MAX_SYMBOLS
    if r1 >= r2 goto define_full
    pop r7, r6, r5, r4
    return
define_check:
    r1 = [r0 + 8]
    if r1 != r6 goto define_moved
    r1 = [r0 + 12]
    if r1 != r7 goto define_moved
    pop r7, r6, r5, r4
    return
define_twice:
    r0 = message_defined_twice
    r1 = r4
    r2 = r5
    call fail_name
define_full:
    r0 = message_too_many_symbols
    call fail
define_moved:
    r0 = message_moved
    r1 = r4
    r2 = r5
    call fail_name

; Returns r0 = the address of the label symbol at r0. Changes only r0-r2.
label_address:
    r1 = [r0 + 8]
    r0 = [r0 + 12]
    r2 = S_BSS
    if r1 == r2 goto label_address_bss
    r1 = GUARD
    r0 += r1
    return
label_address_bss:
    r1 = bss_base
    r1 = [r1]
    r0 += r1
    return

; ---------------------------------------------------------------------------
; Output

; Emits the instruction word r0.
emit:
    r1 = section
    r1 = [r1]
    if r1 != 0 goto emit_not_code
    r1 = offsets
    r2 = [r1]
    r3 = pass
    r3 = [r3]
    r1 = 1
    if r3 == r1 goto emit_counted
    r1 = code
    r1 += r2
    [r1] = r0
emit_counted:
    r2 += 4
    r1 = CODE_SIZE
    if r2 > r1 goto emit_full
    r1 = offsets
    [r1] = r2
    return
emit_not_code:
    r0 = message_not_code
    call fail
emit_full:
    r0 = message_too_much_code
    call fail

; Emits an instruction with opcode r0, fields a = r1, b = r2, and low 16 bits r3.
emit_fields:
    r0 <<= 24
    r1 <<= 20
    r0 |= r1
    r2 <<= 16
    r0 |= r2
    r3 <<= 16
    r3 >>= 16
    r0 |= r3
    goto emit

; Emits a jump-format instruction with opcode r0 and target address r1.
emit_jump:
    r0 <<= 24
    r1 >>= 2
    r1 <<= 8
    r1 >>= 8
    r0 |= r1
    goto emit

; Appends r1 bytes from r0 to the data section.
emit_data:
    r2 = section
    r2 = [r2]
    r3 = 1
    if r2 != r3 goto emit_data_not_data
    r2 = offsets
    r2 = [r2 + 4]
    r3 = DATA_SIZE
    r3 -= r2
    if r1 > r3 goto emit_data_full
    r3 = pass
    r3 = [r3]
    push r1
    r1 = 1
    if r3 == r1 goto emit_data_counted
    r3 = data
    r3 += r2
    pop r1
    push r1
    copy r3, r0, r1
emit_data_counted:
    pop r1
    r2 += r1
    r3 = offsets
    [r3 + 4] = r2
    return
emit_data_not_data:
    r0 = message_not_data
    call fail
emit_data_full:
    r0 = message_too_much_data
    call fail

; Reserves r0 zero bytes in the current section.
zeros:
    r1 = section
    r1 = [r1]
    if r1 == 0 goto zeros_code
    r2 = 1
    if r1 != r2 goto zeros_bss
    r1 = r0
    r0 = zero_page
zeros_data:
    if r1 == 0 goto zeros_done
    r2 = 256
    if r1 < r2 goto zeros_data_last
    push r1
    r1 = 256
    call emit_data
    pop r1
    r1 -= 256
    r0 = zero_page
    goto zeros_data
zeros_data_last:
    call emit_data
zeros_done:
    return
zeros_bss:
    r1 = offsets
    r2 = [r1 + 8]
    r2 += r0
    if r2 < r0 goto zeros_too_big
    [r1 + 8] = r2
    return
zeros_code:
    r0 = message_space_in_code
    call fail
zeros_too_big:
    r0 = message_too_big
    call fail

; ---------------------------------------------------------------------------
; Statements

statement:
    push r4, r5, r6, r7
    call peek
    r3 = T_NAME
    if r0 != r3 goto statement_body
    r2 = cursor
    r2 = [r2]
    r3 = [r2 + TOKEN_SIZE]
    r1 = T_OP
    if r3 != r1 goto statement_body
    r3 = [r2 + 16]
    r1 = OP_COLON
    if r3 != r1 goto statement_body
    r0 = [r2 + 4]
    r1 = [r2 + 8]
    r2 = section
    r2 = [r2]
    r3 = offsets
    r4 = r2 << 2
    r3 += r4
    r3 = [r3]
    r2 += S_CODE
    call define
    call advance
    call advance
statement_body:
    call peek
    if r0 == 0 goto statement_done
    r3 = T_DIR
    if r0 == r3 goto statement_directive
    r3 = T_NAME
    if r0 == r3 goto statement_constant
    r3 = T_REG
    if r0 == r3 goto statement_assign
    r3 = T_OP
    if r0 == r3 goto statement_op
    r3 = T_KEY
    if r0 != r3 goto statement_bad
    r3 = K_U8
    if r1 >= r3 goto statement_store
    call keyword
    goto statement_done
statement_op:
    r3 = OP_LBRACKET
    if r1 != r3 goto statement_bad
statement_store:
    call store
    goto statement_done
statement_directive:
    call directive
    goto statement_done
statement_constant:
    call constant_definition
    goto statement_done
statement_assign:
    call assign
statement_done:
    pop r7, r6, r5, r4
    return
statement_bad:
    r0 = message_expected_statement
    call fail

; NAME = value
constant_definition:
    push r4, r5, r6
    call peek
    r4 = r1
    r5 = r2
    call advance
    r0 = OP_ASSIGN
    call take_op
    if r0 == 0 goto constant_definition_bad
    call constant
    r6 = r0
    call expect_end
    r0 = r4
    r1 = r5
    r2 = S_CONST
    r3 = r6
    call define
    pop r6, r5, r4
    return
constant_definition_bad:
    r0 = message_expected_assign
    call fail

; Statements that start with a register.
assign:
    push r4, r5, r6, r7
    call peek
    r4 = r1                     ; destination
    call advance
    call peek
    r3 = T_OP
    if r0 != r3 goto assign_no_operator
    r5 = r1                     ; operator
    call advance
    r3 = OP_ASSIGN
    if r5 == r3 goto assign_plain
    r0 = assign_ops
    r0 += r5
    r0 = u8 [r0]
    if r0 == 0 goto assign_bad_operator
    r1 = r4
    r2 = r4
    call alu
    goto assign_done
assign_plain:
    r0 = K_IO
    call take_keyword
    if r0 == 0 goto assign_not_io
    call expect_reg
    r5 = r0                     ; port register
    call expect_end
    r0 = 0x90
    r1 = r4
    r2 = r5
    r3 = 0
    call emit_fields
    goto assign_done
assign_not_io:
    call size_prefix
    r5 = r0                     ; size
    r6 = r1                     ; had a prefix?
    call peek
    r3 = T_OP
    if r0 != r3 goto assign_not_load
    r3 = OP_LBRACKET
    if r1 != r3 goto assign_not_load
    call memory_operand
    r6 = r0                     ; base
    r7 = r1                     ; offset
    call expect_end
    r0 = r5 + 0x30
    r1 = r4
    r2 = r6
    r3 = r7
    call emit_fields
    goto assign_done
assign_not_load:
    if r6 != 0 goto assign_expected_bracket
    r3 = T_REG
    if r0 != r3 goto assign_value
    r5 = r1                     ; source register
    call advance
    call peek
    if r0 != 0 goto assign_binary
    r0 = 0x20
    r1 = r4
    r2 = r5
    r3 = 0
    call emit_fields
    goto assign_done
assign_binary:
    r3 = T_OP
    if r0 != r3 goto assign_no_operator
    r0 = binary_ops
    r0 += r1
    r0 = u8 [r0]
    if r0 == 0 goto assign_no_operator
    push r0
    call advance
    pop r0
    r1 = r4
    r2 = r5
    call alu
    goto assign_done
assign_value:
    call atom
    r5 = r0                     ; value
    r6 = 0                      ; one instruction if it fits
    if r1 == 0 goto assign_value_short
    if r2 == 0 goto assign_value_end
    r3 = [r2 + 8]
    r1 = S_CONST
    if r3 != r1 goto assign_value_end
assign_value_short:
    r6 = 1
assign_value_end:
    call expect_end
    r0 = 1
    r1 = r4
    r2 = 0
    r3 = r5
    call emit_fields
    if r6 == 0 goto assign_value_high
    r0 = r5 + 0x7fff
    r0 += 1
    r1 = 0xffff
    if r0 <= r1 goto assign_done
assign_value_high:
    r0 = 2
    r1 = r4
    r2 = 0
    r3 = r5 >> 16
    call emit_fields
assign_done:
    pop r7, r6, r5, r4
    return
assign_no_operator:
    r0 = message_expected_operator
    call fail
assign_bad_operator:
    r0 = message_unknown_operator
    call fail
assign_expected_bracket:
    r0 = message_expected
    r1 = bracket_text
    r2 = 1
    call fail_name

; Emits arithmetic operation r0 with destination r1 and first operand r2.
; The second operand, a register or a constant, is at the cursor.
alu:
    push r4, r5, r6, r7
    r4 = r0
    r5 = r1
    r6 = r2
    call peek
    r3 = T_REG
    if r0 != r3 goto alu_immediate
    r7 = r1
    call advance
    call expect_end
    r0 = r4
    r1 = r5
    r2 = r6
    r3 = r7 << 12
    call emit_fields
    pop r7, r6, r5, r4
    return
alu_immediate:
    call immediate
    r7 = r0
    call expect_end
    r0 = r4 + 0x10
    r1 = r5
    r2 = r6
    r3 = r7
    call emit_fields
    pop r7, r6, r5, r4
    return

; Reads "[reg]", "[reg + value]" or "[reg - value]".
; Returns r0 = the base register and r1 = the offset's low 16 bits.
memory_operand:
    push r4, r5
    r0 = OP_LBRACKET
    call expect_op
    call expect_reg
    r4 = r0
    r5 = 0
    r0 = OP_PLUS
    call take_op
    if r0 != 0 goto memory_operand_plus
    r0 = OP_MINUS
    call take_op
    if r0 == 0 goto memory_operand_close
    call constant
    r0 ^= -1
    r0 += 1
    goto memory_operand_check
memory_operand_plus:
    call constant
memory_operand_check:
    r5 = r0
    r0 += 0x7fff
    r0 += 1
    r1 = 0xffff
    if r0 > r1 goto memory_operand_big
memory_operand_close:
    r0 = OP_RBRACKET
    call expect_op
    r0 = r4
    r1 = r5 << 16
    r1 >>= 16
    pop r5, r4
    return
memory_operand_big:
    r0 = message_too_big
    call fail

; [reg + offset] = reg, with an optional size prefix.
store:
    push r4, r5, r6, r7
    call size_prefix
    r4 = r0
    call memory_operand
    r5 = r0
    r6 = r1
    r0 = OP_ASSIGN
    call expect_op
    call expect_reg
    r7 = r0
    call expect_end
    r0 = r4 + 0x34
    r1 = r7
    r2 = r5
    r3 = r6
    call emit_fields
    pop r7, r6, r5, r4
    return

; Statements that start with a keyword.
keyword:
    push r4, r5, r6, r7
    call peek
    r4 = r1
    call advance
    r3 = K_GOTO
    if r4 == r3 goto keyword_goto
    r3 = K_CALL
    if r4 == r3 goto keyword_call
    r3 = K_RETURN
    if r4 == r3 goto keyword_return
    r3 = K_HALT
    if r4 == r3 goto keyword_halt
    r3 = K_SYS
    if r4 == r3 goto keyword_sys
    r3 = K_PUSH
    if r4 == r3 goto keyword_push
    r3 = K_POP
    if r4 == r3 goto keyword_pop
    r3 = K_COPY
    if r4 == r3 goto keyword_copy
    r3 = K_FILL
    if r4 == r3 goto keyword_fill
    r3 = K_RESUME
    if r4 == r3 goto keyword_resume
    r3 = K_WAIT
    if r4 == r3 goto keyword_wait
    r3 = K_IO
    if r4 == r3 goto keyword_io
    goto keyword_if

; io port = value
keyword_io:
    call expect_reg
    r4 = r0                     ; port register
    r0 = OP_ASSIGN
    call expect_op
    call expect_reg
    r5 = r0                     ; value register
    call expect_end
    r0 = 0x91
    r1 = r5
    r2 = r4
    r3 = 0
    call emit_fields
    goto keyword_done

keyword_goto:
    call code_label
    r4 = r0
    call expect_end
    r0 = 0x50
    r1 = r4
    call emit_jump
    goto keyword_done

keyword_call:
    call peek
    r3 = T_REG
    if r0 != r3 goto keyword_call_label
    r4 = r1
    call advance
    call expect_end
    r0 = 0x52
    r1 = r4
    r2 = 0
    r3 = 0
    call emit_fields
    goto keyword_done
keyword_call_label:
    call code_label
    r4 = r0
    call expect_end
    r0 = 0x51
    r1 = r4
    call emit_jump
    goto keyword_done

keyword_return:
    r4 = 0x53
    goto keyword_bare
keyword_resume:
    r4 = 0x92
    goto keyword_bare
keyword_wait:
    r4 = 0x93
    goto keyword_bare
keyword_halt:
    r4 = 0xff
keyword_bare:
    call expect_end
    r0 = r4
    r1 = 0
    r2 = 0
    r3 = 0
    call emit_fields
    goto keyword_done

keyword_sys:
    call constant
    r4 = r0
    call expect_end
    r0 = 0xffff
    if r4 > r0 goto keyword_sys_bad
    r0 = 0x80
    r1 = 0
    r2 = 0
    r3 = r4
    call emit_fields
    goto keyword_done
keyword_sys_bad:
    r0 = message_bad_sys
    call fail

keyword_push:
    r5 = 0x60
    goto keyword_registers
keyword_pop:
    r5 = 0x61
keyword_registers:
    call expect_reg
    r1 = r0
    r0 = r5
    r2 = 0
    r3 = 0
    call emit_fields
    call peek
    if r0 == 0 goto keyword_done
    r0 = OP_COMMA
    call expect_op
    goto keyword_registers

keyword_copy:
    r5 = 0x70
    goto keyword_three
keyword_fill:
    r5 = 0x71
keyword_three:
    call expect_reg
    r4 = r0
    r0 = OP_COMMA
    call expect_op
    call expect_reg
    r6 = r0
    r0 = OP_COMMA
    call expect_op
    call expect_reg
    r7 = r0
    call expect_end
    r0 = r5
    r1 = r4
    r2 = r6
    r3 = r7 << 12
    call emit_fields
    goto keyword_done

; if a CMP b goto label
keyword_if:
    r3 = K_IF
    if r4 != r3 goto keyword_bad
    call expect_reg
    r4 = r0                     ; a
    call peek
    r3 = T_OP
    if r0 != r3 goto keyword_if_no_comparison
    r0 = compare_ops
    r0 += r1
    r5 = u8 [r0]                ; comparison code
    if r5 == 0 goto keyword_if_no_comparison
    call advance
    call peek
    r3 = T_NUM
    if r0 != r3 goto keyword_if_register
    if r1 != 0 goto keyword_if_zero_only
    r0 = 1
    if r5 == r0 goto keyword_if_zero
    r0 = 2
    if r5 != r0 goto keyword_if_zero_only
keyword_if_zero:
    r5 += 0x45                  ; == gives 0x46, != gives 0x47
    r6 = 0                      ; b
    call advance
    goto keyword_if_goto
keyword_if_register:
    call expect_reg
    r6 = r0                     ; b
    r0 = r5 >> 4
    if r0 == 0 goto keyword_if_ordered
    r0 = r4
    r4 = r6
    r6 = r0
keyword_if_ordered:
    r5 &= 15
    r5 += 0x3f
keyword_if_goto:
    r0 = K_GOTO
    call take_keyword
    if r0 == 0 goto keyword_if_no_goto
    call code_label
    r7 = r0
    call expect_end
    r0 = pass
    r0 = [r0]
    r1 = 1
    r3 = 0
    if r0 == r1 goto keyword_if_emit
    r0 = offsets
    r0 = [r0]
    r1 = GUARD
    r0 += r1
    r0 += 4
    r3 = r7 - r0
    r3 >>s= 2
    r0 = r3 + 0x7fff
    r0 += 1
    r1 = 0xffff
    if r0 > r1 goto keyword_if_far
keyword_if_emit:
    r0 = r5
    r1 = r4
    r2 = r6
    call emit_fields
keyword_done:
    pop r7, r6, r5, r4
    return
keyword_bad:
    r0 = message_expected_statement
    call fail
keyword_if_no_comparison:
    r0 = message_expected_comparison
    call fail
keyword_if_zero_only:
    r0 = message_zero_only
    call fail
keyword_if_no_goto:
    r0 = message_expected_goto
    call fail
keyword_if_far:
    r0 = message_far_branch
    call fail

; Statements that start with a directive.
directive:
    push r4, r5, r6, r7
    call peek
    r4 = r1
    call advance
    r3 = D_BSS
    if r4 > r3 goto directive_not_section
    call expect_end
    r0 = section
    [r0] = r4
    goto directive_done
directive_not_section:
    r3 = D_BYTE
    if r4 == r3 goto directive_byte
    r3 = D_WORD
    if r4 == r3 goto directive_word
    call constant
    r5 = r0
    call expect_end
    r3 = D_ZERO
    if r4 == r3 goto directive_zero
    r3 = D_ALIGN
    if r4 == r3 goto directive_align
    r3 = D_MEMORY
    if r4 == r3 goto directive_memory
    r3 = D_ALLOW
    if r4 == r3 goto directive_allow
    r0 = stack_set
    r1 = [r0]
    if r1 != 0 goto directive_twice
    r1 = 1
    [r0] = r1
    [r0 + 4] = r5
    goto directive_done
directive_allow:
    r0 = allow_set
    r1 = [r0]
    if r1 != 0 goto directive_twice
    r1 = 1
    [r0] = r1
    [r0 + 4] = r5
    goto directive_done
directive_memory:
    r0 = memory_set
    r1 = [r0]
    if r1 != 0 goto directive_twice
    r1 = 1
    [r0] = r1
    [r0 + 4] = r5
    goto directive_done
directive_zero:
    r0 = r5
    call zeros
    goto directive_done
directive_align:
    if r5 == 0 goto directive_bad_size
    r0 = section
    r0 = [r0]
    r0 <<= 2
    r1 = offsets
    r0 += r1
    r0 = [r0]
    r0 %= r5
    if r0 == 0 goto directive_align_pad
    r0 = r5 - r0
directive_align_pad:
    call zeros
    goto directive_done

directive_byte:
    call peek
    r3 = T_STR
    if r0 != r3 goto directive_byte_value
    r0 = r1
    r1 = r2
    call emit_data
    call advance
    goto directive_byte_next
directive_byte_value:
    call constant
    r1 = r0 + 128
    r2 = 384
    if r1 >= r2 goto directive_byte_big
    r1 = byte_buffer
    u8 [r1] = r0
    r0 = r1
    r1 = 1
    call emit_data
directive_byte_next:
    call peek
    if r0 == 0 goto directive_done
    r0 = OP_COMMA
    call expect_op
    goto directive_byte

directive_word:
    r0 = section
    r0 = [r0]
    r1 = 1
    if r0 != r1 goto directive_word_value
    r0 = offsets
    r0 = [r0 + 4]
    r0 &= 3
    if r0 != 0 goto directive_word_unaligned
directive_word_value:
    call atom
    r1 = word_buffer
    [r1] = r0
    r0 = r1
    r1 = 4
    call emit_data
    call peek
    if r0 == 0 goto directive_done
    r0 = OP_COMMA
    call expect_op
    goto directive_word

directive_done:
    pop r7, r6, r5, r4
    return
directive_twice:
    r0 = message_set_twice
    call fail
directive_bad_size:
    r0 = message_bad_size
    call fail
directive_byte_big:
    r0 = message_byte_too_big
    call fail
directive_word_unaligned:
    r0 = message_unaligned_word
    call fail

; ---------------------------------------------------------------------------
; Writing the results

write_executable:
    push r4, r5, r6, r7
    r0 = start_name
    r1 = 5
    call find_symbol
    if r1 == 0 goto write_executable_no_start
    r1 = [r0 + 8]
    r2 = S_CODE
    if r1 != r2 goto write_executable_no_start
    call label_address
    r4 = header
    [r4 + 4] = r0
    r0 = 0x32585254             ; "TRX2"
    [r4] = r0
    r0 = allow_set
    r0 = [r0 + 4]
    [r4 + 28] = r0
    r1 = offsets
    r5 = [r1]
    [r4 + 8] = r5               ; code
    r6 = [r1 + 4]
    [r4 + 12] = r6              ; data
    r7 = [r1 + 8]
    [r4 + 16] = r7              ; bss
    r0 = DEFAULT_STACK
    r1 = stack_set
    r2 = [r1]
    if r2 == 0 goto write_executable_stack
    r0 = [r1 + 4]
write_executable_stack:
    [r4 + 20] = r0
    r6 += 3                     ; need = code + align4(data) + align4(bss) + stack
    r6 >>= 2
    r6 <<= 2
    r7 += 3
    r7 >>= 2
    r7 <<= 2
    r5 += r6
    r5 += r7
    r5 += r0
    r0 = r5
    r1 = memory_set
    r2 = [r1]
    if r2 == 0 goto write_executable_memory
    r0 = [r1 + 4]
write_executable_memory:
    r0 += 3
    r0 >>= 2
    r0 <<= 2
    if r0 < r5 goto write_executable_small
    [r4 + 24] = r0

    r0 = output_name
    r1 = [r0 + 4]
    r0 = [r0]
    r2 = 1
    sys SYS_OPEN
    r1 = -1
    if r0 == r1 goto write_executable_cant
    r5 = r0
    r1 = header
    r2 = HEADER_SIZE
    call write_all
    r0 = r5
    r1 = code
    r2 = offsets
    r2 = [r2]
    call write_all
    r0 = r5
    r1 = data
    r2 = offsets
    r2 = [r2 + 4]
    call write_all
    r0 = r5
    sys SYS_CLOSE
    pop r7, r6, r5, r4
    return
write_executable_no_start:
    r0 = message_no_start
    call fatal
write_executable_small:
    r0 = message_small_memory
    call fatal
write_executable_cant:
    r0 = message_cant_write
    r1 = output_name
    r2 = [r1 + 4]
    r1 = [r1]
    call fatal_name

; Writes "c 00010000 name" for every label, when -s was given.
write_symbols:
    push r4, r5, r6, r7
    r0 = symbols_name
    r1 = [r0 + 4]
    r0 = [r0]
    if r0 == 0 goto write_symbols_done
    r2 = 1
    sys SYS_OPEN
    r1 = -1
    if r0 == r1 goto write_symbols_cant
    r4 = r0                     ; file
    r5 = symbols                ; entry
    r6 = SYMBOL_MASK
    r6 += 1
    r6 *= SYMBOL_SIZE
    r6 += r5                    ; end of the table
write_symbols_next:
    if r5 == r6 goto write_symbols_close
    r0 = [r5]
    if r0 == 0 goto write_symbols_skip
    r0 = [r5 + 8]
    r1 = S_CONST
    if r0 == r1 goto write_symbols_skip
    r7 = line_buffer
    r1 = 'd'
    r2 = S_CODE
    if r0 != r2 goto write_symbols_kind
    r1 = 'c'
write_symbols_kind:
    u8 [r7] = r1
    r1 = ' '
    u8 [r7 + 1] = r1
    r0 = r5
    call label_address
    r1 = r7 + 2
    call format_hex
    r1 = ' '
    u8 [r7 + 10] = r1
    r0 = r7 + 11
    r1 = [r5]
    r2 = [r5 + 4]
    r3 = 200
    if r2 > r3 goto write_symbols_skip
    copy r0, r1, r2
    r0 += r2
    r1 = '\n'
    u8 [r0] = r1
    r2 += 12
    r0 = r4
    r1 = r7
    call write_all
write_symbols_skip:
    r5 += SYMBOL_SIZE
    goto write_symbols_next
write_symbols_close:
    r0 = r4
    sys SYS_CLOSE
write_symbols_done:
    pop r7, r6, r5, r4
    return
write_symbols_cant:
    r0 = message_cant_write
    r1 = symbols_name
    r2 = [r1 + 4]
    r1 = [r1]
    call fatal_name

; Writes r0 as eight hex digits at r1.
format_hex:
    r2 = 8
format_hex_next:
    if r2 == 0 goto format_hex_done
    r2 -= 1
    r3 = r0 & 15
    r0 >>= 4
    push r0
    r0 = hex_digits
    r0 += r3
    r0 = u8 [r0]
    r3 = r1 + r2
    u8 [r3] = r0
    pop r0
    goto format_hex_next
format_hex_done:
    return

; Writes r2 bytes from r1 to file r0.
write_all:
    if r2 == 0 goto write_all_done
    r3 = r0
    sys SYS_WRITE
    if r0 == 0 goto write_all_failed
    if r0 > r2 goto write_all_failed
    r1 += r0
    r2 -= r0
    r0 = r3
    goto write_all
write_all_done:
    return
write_all_failed:
    r0 = 1
    sys SYS_EXIT

; ---------------------------------------------------------------------------
; Errors

; Prints "file:line: message" and exits. r0 = the message (zero-terminated).
fail:
    r1 = 0
    r2 = 0

; Prints "file:line: message 'name'" and exits. r1 = name, r2 = its length.
fail_name:
    r4 = r0
    r5 = r1
    r6 = r2
    r0 = current_file
    r0 = [r0]
    r1 = [r0]
    r2 = [r0 + 4]
    r0 = STDERR
    call write_all
    r1 = colon
    call print_text
    r0 = file_line
    r0 = [r0]
    call print_decimal
    r1 = colon_space
    call print_text
    goto fatal_message

; Prints "asm: message" and exits.
fatal:
    r1 = 0
    r2 = 0

; Prints "asm: message 'name'" and exits.
fatal_name:
    r4 = r0
    r5 = r1
    r6 = r2
    r1 = program_name
    call print_text
fatal_message:
    r1 = r4
    call print_text
    if r6 == 0 goto fatal_end
    r1 = space_quote
    call print_text
    r0 = STDERR
    r1 = r5
    r2 = r6
    call write_all
    r1 = quote
    call print_text
    ; A compiled name like _write that nothing defines usually means the
    ; runtime was left out.
    r1 = message_undefined
    if r4 != r1 goto fatal_end
    r0 = u8 [r5]
    r1 = '_'
    if r0 != r1 goto fatal_end
    r1 = message_runtime
    call print_text
fatal_end:
    r1 = newline
    call print_text
    r0 = 1
    sys SYS_EXIT

; Prints the zero-terminated text at r1 to standard error.
print_text:
    r2 = 0
print_text_length:
    r0 = r1 + r2
    r0 = u8 [r0]
    if r0 == 0 goto print_text_write
    r2 += 1
    goto print_text_length
print_text_write:
    r0 = STDERR
    goto write_all

; Prints r0 in decimal to standard error.
print_decimal:
    r1 = number_end
    r2 = 10
print_decimal_next:
    r1 -= 1
    r3 = r0 % r2
    r3 += '0'
    u8 [r1] = r3
    r0 /= r2
    if r0 != 0 goto print_decimal_next
    r2 = number_end
    r2 -= r1
    r0 = STDERR
    goto write_all

; ---------------------------------------------------------------------------
; Tables

.data

operators:
    .byte 4, ">>s="
    .byte 3, "<<=", 3, ">>=", 3, "/s=", 3, "%s=", 3, ">>s", 3, "<=s", 3, ">=s", 3, "*f="
    .byte 3, "/f="
    .byte 2, "<<", 2, ">>", 2, "==", 2, "!=", 2, "<=", 2, ">=", 2, "+=", 2, "-="
    .byte 2, "*=", 2, "/=", 2, "%=", 2, "&=", 2, "|=", 2, "^=", 2, "/s", 2, "%s"
    .byte 2, "<s", 2, ">s", 2, "*f", 2, "/f"
    .byte 1, "+", 1, "-", 1, "*", 1, "/", 1, "%", 1, "&", 1, "|", 1, "^"
    .byte 1, "<", 1, ">", 1, "=", 1, "[", 1, "]", 1, ",", 1, ":"
    .byte 0

; For each operator id: its arithmetic opcode as a binary operator, or 0.
binary_ops:
    .byte 0, 0, 0, 0, 0, 0x1c, 0, 0, 0, 0, 0x1a, 0x1b, 0, 0, 0
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x14, 0x16, 0, 0, 0x1d, 0x1e
    .byte 0x10, 0x11, 0x12, 0x13, 0x15, 0x17, 0x18, 0x19, 0, 0, 0, 0, 0, 0, 0

; For each operator id: its arithmetic opcode as a compound assignment, or 0.
assign_ops:
    .byte 0x1c, 0x1a, 0x1b, 0x14, 0x16, 0, 0, 0, 0x1d, 0x1e, 0, 0, 0, 0, 0
    .byte 0, 0x10, 0x11, 0x12, 0x13, 0x15, 0x17, 0x18, 0x19, 0, 0, 0, 0, 0, 0
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

; For each operator id: 0, or a branch code. The low four bits are the branch
; opcode minus 0x3f; 16 is added when the operands swap.
compare_ops:
    .byte 0, 0, 0, 0, 0, 0, 0x16, 0x06, 0, 0, 0, 0, 0x01, 0x02, 0x14
    .byte 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05, 0x15, 0, 0
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x13, 0, 0, 0, 0, 0

keywords:
    .byte 2, "if", 4, "goto", 4, "call", 6, "return", 4, "push", 3, "pop"
    .byte 4, "copy", 4, "fill", 3, "sys", 4, "halt", 2, "io", 6, "resume", 4, "wait"
    .byte 2, "u8", 3, "u16", 3, "u32"
    .byte 0

directives:
    .byte 4, "code", 4, "data", 3, "bss", 4, "byte", 4, "word", 4, "zero"
    .byte 5, "align", 6, "memory", 5, "stack", 5, "allow"
    .byte 0

escapes:
    .byte 'n', 10, 't', 9, 'r', 13, '0', 0, '\\', 92, '\'', 39, '"', 34, 0

; 2 for letters and '_', which start names; 1 for digits and '.', which
; only continue them, as in _io.write; 0 for everything else.
char_classes:
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0
    .byte 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0
    .byte 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    .byte 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 2
    .byte 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    .byte 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0
    .zero 128

; The value of each hex digit, or 255.
digit_values:
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 255, 255, 255, 255, 255, 255
    .byte 255, 10, 11, 12, 13, 14, 15, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 10, 11, 12, 13, 14, 15, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    .byte 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255

hex_digits:
    .byte "0123456789abcdef"
option_o:
    .byte "-o"
option_s:
    .byte "-s"
start_name:
    .byte "start"
bracket_text:
    .byte "["

program_name:
    .byte "asm: ", 0
colon:
    .byte ":", 0
colon_space:
    .byte ": ", 0
space_quote:
    .byte " '", 0
quote:
    .byte "'", 0
newline:
    .byte "\n", 0

message_usage:
    .byte "usage: asm -o program.bin [-s program.sym] file.asm...", 0
message_long_arguments:
    .byte "arguments are too long", 0
message_too_many_files:
    .byte "too many input files", 0
message_cant_read:
    .byte "can't read", 0
message_cant_write:
    .byte "can't write", 0
message_too_much_source:
    .byte "too much source", 0
message_long_line:
    .byte "line is too long", 0
message_unknown_directive:
    .byte "unknown directive", 0
message_unterminated_char:
    .byte "unterminated character", 0
message_unterminated_string:
    .byte "unterminated string", 0
message_unexpected_character:
    .byte "unexpected character", 0
message_bad_number:
    .byte "bad number", 0
message_bad_escape:
    .byte "bad escape", 0
message_expected:
    .byte "expected", 0
message_expected_register:
    .byte "expected a register", 0
message_trailing_text:
    .byte "unexpected text at end of line", 0
message_expected_value:
    .byte "expected a value", 0
message_undefined:
    .byte "undefined name", 0
message_runtime:
    .byte " (programs need /lib/rt.asm first)", 0
message_early_constant:
    .byte "constant used before its definition:", 0
message_negated_label:
    .byte "a label can't be negated", 0
message_expected_constant:
    .byte "expected a number or constant", 0
message_too_big:
    .byte "value doesn't fit", 0
message_expected_label:
    .byte "expected a label", 0
message_not_code_label:
    .byte "not a label in .code:", 0
message_defined_twice:
    .byte "already defined:", 0
message_too_many_symbols:
    .byte "too many symbols", 0
message_moved:
    .byte "internal error, symbol moved between passes:", 0
message_not_code:
    .byte "instructions belong in .code", 0
message_too_much_code:
    .byte "too much code", 0
message_not_data:
    .byte "data belongs in .data", 0
message_too_much_data:
    .byte "too much data", 0
message_space_in_code:
    .byte "can't reserve space in .code", 0
message_expected_statement:
    .byte "expected a statement", 0
message_expected_assign:
    .byte "expected '=' after a name", 0
message_expected_operator:
    .byte "expected an operator", 0
message_unknown_operator:
    .byte "unknown operator", 0
message_bad_sys:
    .byte "system call number out of range", 0
message_expected_comparison:
    .byte "expected a comparison", 0
message_zero_only:
    .byte "only '== 0' and '!= 0' compare with a number", 0
message_expected_goto:
    .byte "expected 'goto'", 0
message_far_branch:
    .byte "branch target is too far away", 0
message_set_twice:
    .byte "directive is already set", 0
message_bad_size:
    .byte "bad size", 0
message_byte_too_big:
    .byte "value doesn't fit in a byte", 0
message_unaligned_word:
    .byte ".word needs 4-byte alignment (use .align 4)", 0
message_no_start:
    .byte "no 'start' label in .code", 0
message_small_memory:
    .byte ".memory is smaller than the program", 0

; ---------------------------------------------------------------------------
; State

.bss

pass:
    .zero 4
section:
    .zero 4
offsets:
    .zero 12                    ; code, data and bss
bss_base:
    .zero 4
line:
    .zero 4                     ; counts every line of every file
file_line:
    .zero 4
current_file:
    .zero 4
memory_set:
    .zero 8                     ; set?, value
stack_set:
    .zero 8
allow_set:
    .zero 8
symbol_count:
    .zero 4
cursor:
    .zero 4
byte_buffer:
    .zero 4
word_buffer:
    .zero 4
output_name:
    .zero 8                     ; text, length
symbols_name:
    .zero 8
file_count:
    .zero 4
files:
    .zero 1024                  ; MAX_FILES * FILE_SIZE: name, name length, start, end
header:
    .zero 32
zero_page:
    .zero 256
number_buffer:
    .zero 12
number_end:
line_buffer:
    .zero 256

tokens:
    .zero 756                   ; 63 tokens
tokens_last:
    .zero 12                    ; room for the final T_END
strings:
    .zero 1024
strings_end:

argument_text:
    .zero 0x4000
argument_text_end:

symbols:
    .zero 0x140000              ; 0x10000 * SYMBOL_SIZE
source:
    .zero 0x800000
source_end:
code:
    .zero 0x400000
data:
    .zero 0x200000

.memory 0x1000000
.stack 0x4000
.allow 3                       ; read and write files

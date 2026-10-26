# The Terrarium File System

The disk is split into 4096-byte blocks. Block 0 describes the disk, a table
after it links blocks into files, and directories are files that list other
files.

## Block 0

| Offset | Field       | Meaning                                             |
| ------ | ----------- | --------------------------------------------------- |
| 0      | magic       | the bytes `TFS1`                                    |
| 4      | block size  | 4096                                                |
| 8      | blocks      | blocks on the disk                                  |
| 12     | table       | first block of the table                            |
| 16     | table size  | blocks in the table                                 |
| 20     | root        | first block of the root directory                   |
| 24     | root size   | size of the root directory in bytes                 |
| 28     | kernel      | first block of the kernel                           |
| 32     | kernel size | size of the kernel in bytes                         |

All fields are little-endian 32-bit words.

## The table

One 32-bit word per block, saying what comes after it:

| Value        | Meaning                                  |
| ------------ | ---------------------------------------- |
| `0`          | the block is free                        |
| `0xffffffff` | the last block of a file                 |
| `0xfffffffe` | used by block 0 or the table itself      |
| anything else | the file's next block                   |

A file is its first block and the chain that follows. An empty file has no
blocks, and its first block is 0.

## Directories

A directory's contents are 64-byte entries:

| Offset | Field | Meaning                                         |
| ------ | ----- | ----------------------------------------------- |
| 0      | name  | up to 48 bytes, padded with zeros               |
| 48     | kind  | 0 unused, 1 file, 2 directory                   |
| 52     | first | the first block                                 |
| 56     | size  | the size in bytes                               |
| 60     | —     | reserved, zero                                  |

A name can't contain `/` or a zero byte, and can't be `.` or `..`. Removing a
file marks its entry unused, and new entries fill unused ones first.

## The kernel

The kernel is the file `/system/kernel`. Block 0 also records where it is,
and its blocks are kept in one unbroken run, so the machine's firmware can
load it without understanding the table.

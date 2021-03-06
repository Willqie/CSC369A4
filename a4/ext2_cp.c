#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include "path.h"
#include "ext2.h"


unsigned char *disk;

int main(int argc, char** argv) {
    
    if(argc != 4) {
        fprintf(stderr, "Usage: ext2_cp <image file name> <path to source file> <path to dest>\n");
        exit(1);
    }

    // open source file
    int fd_s = open(argv[2], 'r');
    if(fd_s == -1){
        perror("open");
        return -ENOENT;
    }


    // get source file size
    struct stat st;
    stat(argv[2], &st);

    // check if the file to copy is regular file
    if((st.st_mode & S_IFMT) != S_IFREG){
        fprintf(stderr, "Source file is not regular file.\n");
        return -ENOENT;
    }
    // check the size of the file to copy
    if(st.st_size > 12*1024 + 256*1024){
        fprintf(stderr, "Source file is too large.\n");
        exit(-ENOSPC);
    }


    // open disk image
    int fd = open(argv[1], O_RDWR);
	if(fd == -1) {
		perror("open");
		exit(1);
    }

    disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(disk == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    struct ext2_group_desc *bd = (struct ext2_group_desc*)(disk + (EXT2_BLOCK_SIZE) * 2);

    struct ext2_inode *inodes = 
    (struct ext2_inode*)(disk + (EXT2_BLOCK_SIZE)*bd->bg_inode_table);


    // find destination
    int length;
    char **path = parse_path(argv[3], &length);
    if (path == NULL) {
        return -1;
    }
    int target_directory = trace_path(path, length - 1);
    if (target_directory == -ENOENT) {
        fprintf(stderr, "The path to destination is invalid.\n");
        return -ENOENT;
    }


    // Check whether the file already exist
    int find_result = find_in_inode(target_directory, path[length-1], 'd');
    
    if (find_result > 0 || find_result == ERR_WRONG_TYPE) {
        fprintf(stderr, "File to create already exists.\n");
        return -EEXIST;
    } else if (find_result != -1) {
        //Should never reach here.
        assert(0);
    }


    // Allocate inode for new file
    int new_inode = allocate_inode();
    if (new_inode == ERR_NO_INODE) {
        fprintf(stderr, "There is no free inode.\n");
        return -ENOSPC;
    }


    // Add file to target_directory
    struct ext2_dir_entry *new_entry = create_directory(target_directory, path[length-1]);
    new_entry->inode = new_inode + 1;
    new_entry->file_type = EXT2_FT_REG_FILE;
    

    // setting inode fields for new file
    struct ext2_inode *this_inode = inodes + new_inode;
    this_inode->i_mode = EXT2_S_IFREG;
    this_inode->i_dtime = 0;
    this_inode->i_links_count = 1;
    this_inode->i_size = st.st_size; 
    this_inode->i_blocks = 0;   
    memset(this_inode->i_block, 0, sizeof(unsigned int) * 15);


    // set up i_block and i_blocks
     int size_remain = st.st_size;
    
    // direct blocks
    for(int i = 0; i < 12 && size_remain > 0; i++){

        // allocate new block for file
        int new_block = allocate_block();
        if (new_block == -1) {
            fprintf(stderr, "There is no free block on the disk.\n");
            return -ENOSPC;
        }
        this_inode->i_block[i] = new_block;
        this_inode->i_blocks += 2;

        // read from source
        char buf[1024];
        memset(buf, 0, 1024);
        if(read(fd_s, buf, 1024) < 0){
            perror("read");
            exit(1);
        }

        // write to block
        unsigned char *this_block = disk + EXT2_BLOCK_SIZE * new_block;
        for(int j = 0; j < 1024; j++){
            this_block[j] = buf[j];
        }

        size_remain -= 1024;
    }

    // single indirect block
    if(size_remain > 0){
        
        int level_one = allocate_block();
        if(level_one == -1){
            fprintf(stderr, "There is no free block on the disk. \n");
            return -ENOSPC;
        }
        this_inode->i_block[12] = level_one;
        this_inode->i_blocks += 2;
        
        unsigned char *indirect_block = disk + EXT2_BLOCK_SIZE * level_one;
        memset(indirect_block, 0, EXT2_BLOCK_SIZE);
        int pointer_count = 0;   // Note: already checked file size, so it won't go over 256
        while(size_remain > 0){
            int new_block = allocate_block();
            if (new_block == -1) {
                fprintf(stderr, "There is no free block on the disk. \n");
                return -ENOSPC;
            }

            int *pointer = ((int *)indirect_block) + pointer_count;
            pointer[0] = new_block;
            pointer_count++;
            this_inode->i_blocks += 2;


            // read from source
            char buf[1024];
            memset(buf, 0, 1024);
            if(read(fd_s, buf, 1024) < 0){
                perror("read");
                exit(1);
            }

            // write to block
            unsigned char *this_block = disk + EXT2_BLOCK_SIZE * new_block;
            memset(this_block, 0, EXT2_BLOCK_SIZE);
            for(int j = 0; j < 1024; j++){
                this_block[j] = buf[j];
            }

            size_remain -= 1024;
        }
    }


    close(fd);
    close(fd_s);
    return 0;
}

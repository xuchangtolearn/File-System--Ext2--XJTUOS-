#include<stdio.h>
#include"string.h"
#include"stdlib.h"
#include"time.h"
#include<sys/ioctl.h>
#include<termios.h>
#include<unistd.h>
#include <stdint.h>

#define blocks 4611 //总块数：1+1+1+512+4096
#define blocksize 512 //每块字节数
#define inodesize 64 //索引结点大小
#define data_begin_block 515 //数据开始块1+1+1+512
#define dirsize 261 //目录体最大长度
#define EXT_NAME_LEN 255 //文件名长度
#define DISK_START 0
#define BLOCK_BITMAP 512
#define INODE_BITMAP 1024
#define INODE_TABLE 1536
#define PATH "virdisk.txt" //文件系统

/******************************    数据结构    ****************************************/
typedef struct ext2_group_desc//组描述符 68字节
{
    char bg_volume_name[16]; //卷名
    uint16_t bg_block_bitmap; //保存块位图的块号
    uint16_t bg_inode_bitmap; //保存索引结点位图的块号
    uint16_t bg_inode_table; //索引结点表的起始块号
    uint16_t bg_free_blocks_count; //本组空闲块的个数
    uint16_t bg_free_inodes_count; //本组空闲索引结点的个数
    uint16_t bg_used_dirs_count; //本组目录的个数
    char psw[16];//password
    char bg_pad[24];
}ext2_group_desc;

typedef struct ext2_inode //索引结点64字节
{
    uint16_t i_mode;//文件类型及访问权限
    uint16_t i_blocks; //文件的数据块个数
    uint32_t i_size;//该索引结点指向的数据块所使用的大小(字节)
    time_t i_atime; //访问时间
    time_t i_ctime; //创建时间
    time_t i_mtime; //修改时间
    time_t i_dtime; //删除时间
    uint16_t i_block[8];//指向数据块的指针
    char i_pad[8]; //填充
}ext2_inode;

typedef struct ext2_dir_entry//目录体 最大261字节 最小7字节
{
    uint16_t inode;//索引节点号，从1开始
    uint16_t rec_len;//目录项长度
    uint8_t name_len;//文件名长度
    uint8_t file_type;//文件类型(1:普通文件，2:目录…)
    char name[EXT_NAME_LEN];
    char i_pad[14]; //填充
}ext2_dir_entry;

//定义全局变量
//缓冲区用于读出和写入数据块的相关内容
ext2_group_desc group_desc;//组描述符
ext2_inode inode;//索引结点缓冲区，只在初始化的时候用到
ext2_dir_entry dir;//目录体缓冲区
FILE *fp;//文件指针
uint16_t last_alloc_inode=0;//上次分配的索引节点号
uint16_t last_alloc_block=0;//上次分配的数据块号
uint16_t currentdir_inode;  //当前目录的索引结点号,很重要，每次用这个来获取当前目录
uint16_t fopen_table[16] ; /*文件打开表，最多可以同时打开16个文件*/
char current_path[256];   	 /*当前路径(字符串) */
uint8_t bitbuf[512]; //位图缓冲区 512字节
uint8_t block_buffer[512];//数据块缓冲区

/******************************    底层    ****************************************/
void update_group_desc() /*将内存中的组描述符更新到"硬盘"*/
{
    fseek(fp,DISK_START,SEEK_SET);
    fwrite(&group_desc,sizeof(ext2_group_desc),1,fp);
}
void reload_group_desc() /*载入可能已更新的组描述符*/
{
    fseek(fp,DISK_START,SEEK_SET);
    fread(&group_desc,sizeof(ext2_group_desc),1,fp);
}	
void reload_inode_entry(uint16_t i,ext2_inode* inode_i)  /*载入特定的索引结点*/
{
    fseek(fp,INODE_TABLE+(i-1)*inodesize,SEEK_SET);
    fread(inode_i,inodesize,1,fp);
}
void update_inode_entry(uint16_t i,ext2_inode* inode_i) /*更新特定的索引结点*/
{
    fseek(fp,INODE_TABLE+(i-1)*inodesize,SEEK_SET);
    fwrite(inode_i,inodesize,1,fp);
}

void update_block_bitmap()//更新block位图 
{
    fseek(fp,BLOCK_BITMAP,SEEK_SET);
    fwrite(bitbuf,blocksize,1,fp);
}
void reload_block_bitmap()//载入block位图 
{
    fseek(fp,BLOCK_BITMAP,SEEK_SET);
    fread(bitbuf,blocksize,1,fp);
}
void update_inode_bitmap()//更新inode位图 
{
    fseek(fp,INODE_BITMAP,SEEK_SET);
    fwrite(bitbuf,blocksize,1,fp);
}
void reload_inode_bitmap()//载入inode位图 
{
    fseek(fp,INODE_BITMAP,SEEK_SET);
    fread(bitbuf,blocksize,1,fp);
}
void reload_block_entry(uint16_t i,uint8_t* block)
{//将第i个数据块读到缓冲区block中
    fseek(fp,data_begin_block*blocksize+i*blocksize,SEEK_SET);
    fread(block,blocksize,1,fp);
}
void update_block_entry(uint16_t i,uint8_t* block)
{//将缓冲区block的内容写入第i个数据块
    fseek(fp,data_begin_block*blocksize+i*blocksize,SEEK_SET);
    fwrite(block,blocksize,1,fp);
}

uint16_t alloc_block()//分配一个数据块,返回数据块号 
{
    uint16_t cur=0;
    uint8_t con=128;
    int offset=0;//数据块在数据块位图中某一字节的第几位
    if(group_desc.bg_free_blocks_count==0)
    {
        printf("There is no block to be alloced!\n");
        return -1;
    }
    reload_block_bitmap();
    cur=cur/8;
    while(bitbuf[cur]==255)
    {
        if(cur==511)
        {
            printf("ERROR\n");
            return -1;
        }
        else cur++;
    }
    while(bitbuf[cur]&con)
    {
        con = con >> 1;;//右移一位
        offset++;
    }
    bitbuf[cur]=bitbuf[cur]+con;
    last_alloc_block=cur*8+offset;
    update_block_bitmap();
    group_desc.bg_free_blocks_count--;
    update_group_desc();
    return last_alloc_block;
}
void free_block(uint16_t block_num)
{
    reload_block_bitmap();
    uint8_t con=128;
    int offset;//数据块在数据块位图中某一字节的第几位
    int byte_pos=block_num/8;//数据块在数据块位图中第几个字节中
    for(offset=0;offset<block_num%8;offset++)
    {
        con = con >> 1;//右移一位
    }
    if(bitbuf[byte_pos]&con)
    {
        bitbuf[byte_pos]=bitbuf[byte_pos]-con;
        reload_block_entry(block_num,block_buffer);
        for(int i=0;i<blocksize;i++)
            block_buffer[i]=0;
        update_block_entry(block_num,block_buffer);//将数据块清零
        update_block_bitmap();
        group_desc.bg_free_blocks_count++;
        update_group_desc();
    }
    else{
        printf("%d:the block to be freed has not been allocated.\n",block_num);
    }
}

/*根据多级索引机制获取数据块号*/
uint16_t get_index_one(uint16_t i,uint16_t first_index)  /*返回一级索引的数据块号*/
{//first_index=inode.i_block[6]
    if(i>=6&&i<6+256)
    {
        uint8_t tmp_block[512];
        uint16_t block_num;
        reload_block_entry(first_index,tmp_block);
        memcpy(&block_num,tmp_block+2*(i-6),2);
        return block_num;
    }
    else printf("错误使用一级索引");
}
uint16_t get_index_two(uint16_t i,uint16_t two_index)  /*返回二级索引的数据块号*/
{//two_index=inode.i_block[7]
    if(i>=6+256&&i<4096)
    {
        int block_num,tmp_num;
        uint8_t tmp_block1[512],tmp_block2[512];
        reload_block_entry(two_index,tmp_block1);
        memcpy(&tmp_num,tmp_block1+2*(i-6-256)/256,2);
        reload_block_entry(two_index,tmp_block2);
        memcpy(&block_num,tmp_block1+2*((i-6-256)%256),2);
        return block_num;
    }
    else printf("错误使用二级索引");
}
void reload_block_i(uint16_t i,unsigned char tmp_block[512],ext2_inode tmp_inode)
{
    if(i<6)
        reload_block_entry(tmp_inode.i_block[i],tmp_block);
    else if(i>=6&&i<6+256)
        reload_block_entry(get_index_one(i,tmp_inode.i_block[6]),tmp_block);
    else 
        reload_block_entry(get_index_two(i,tmp_inode.i_block[7]),tmp_block);
}
void reload_dir_i(uint16_t i,ext2_dir_entry* dir_i,uint16_t offset,ext2_inode tmp_inode)
{//载入当前索引节点第i个数据块中的某个目录项到缓冲区中
    uint8_t tmp_block[512];
    reload_block_i(i,tmp_block,tmp_inode);
    memcpy(dir_i,tmp_block+offset,dirsize);
}
void update_dir_i(uint16_t i,ext2_dir_entry* dir_i,uint16_t offset,ext2_inode tmp_inode)
{//将某个目录项写入当前索引结点的某个数据块中
    uint8_t tmp_block[512];
    memcpy(tmp_block+offset,dir_i,dir_i->rec_len);
    if(i<6)
    {
        reload_block_entry(tmp_inode.i_block[i],tmp_block);
        memcpy(tmp_block+offset,dir_i,dir_i->rec_len);
        update_block_entry(tmp_inode.i_block[i],tmp_block);
    }
        
    else if(i>=6&&i<6+256)
    {
        reload_block_entry(get_index_one(i,tmp_inode.i_block[6]),tmp_block);
        memcpy(tmp_block+offset,dir_i,dir_i->rec_len);
        update_block_entry(get_index_one(i,tmp_inode.i_block[6]),tmp_block);
    }
    else 
    {
        reload_block_entry(get_index_two(i,tmp_inode.i_block[7]),tmp_block);
        memcpy(tmp_block+offset,dir_i,dir_i->rec_len);
        update_block_entry(get_index_two(i,tmp_inode.i_block[7]),tmp_block);
    }
}
void update_index_one(uint16_t i,uint16_t block_num,ext2_inode* tmp_inode)
{//将当前结点的第i个数据块号写入一级索引块
    if(i==6)
    {
        tmp_inode->i_block[6]=alloc_block();
    }
    if(i>=6&&i<6+256)
    {
        uint8_t tmp_block[512];
        reload_block_entry(tmp_inode->i_block[6],tmp_block);
        memcpy(tmp_block+(i-6)*2,&block_num,2);
        update_block_entry(tmp_inode->i_block[6],tmp_block);
    }
    else printf("错误使用一级索引");
}
void update_index_two(uint16_t i,uint16_t block_num,ext2_inode* tmp_inode)  
{//将当前结点的第i个数据块号写入二级索引块
    if(i>=6+256&&i<4096)
    {
        int tmp_num;
        if(i==6+256)
        {
            tmp_inode->i_block[7]=alloc_block();
        }
        if((i-6)%256==0)
        {
            tmp_num=alloc_block();
            uint8_t tmp_block[512];
            reload_block_entry(tmp_inode->i_block[7],tmp_block);
            memcpy(tmp_block+2*(i-6-256)/256,&tmp_num,2);
            update_block_entry(tmp_inode->i_block[7],tmp_block);
        }
        else{
            uint8_t tmp_block[512];
            reload_block_entry(tmp_inode->i_block[7],tmp_block);
            memcpy(&tmp_num,tmp_block+2*(i-6-256)/256,2);
        }
        if(i>=6+256&&i<4096)
        {
            uint8_t tmp_block[512];
            reload_block_entry(tmp_num,tmp_block);
            memcpy(tmp_block+2*((i-6-256)%256),&block_num,2);
            update_block_entry(tmp_num,tmp_block);
        }
    }
    else printf("错误使用二级索引");
}
void update_inode_newblock(uint16_t block_num,ext2_inode* tmp_inode)
{//将新分配的数据块记录到索引结点的信息中
    if(tmp_inode->i_blocks<6)
    {
        tmp_inode->i_block[tmp_inode->i_blocks]=block_num;
    }
    else if(tmp_inode->i_blocks>=6&&tmp_inode->i_blocks<6+256)
    {
        update_index_one(tmp_inode->i_blocks,block_num,tmp_inode);
    }
    else if(tmp_inode->i_blocks>=6+256)
    {
        update_index_two(tmp_inode->i_blocks,block_num,tmp_inode);
    }
    tmp_inode->i_blocks++;
}
void delete_inode_allblock(uint16_t inode_num)
{
    ext2_inode tmp_inode;
    reload_inode_entry(inode_num,&tmp_inode);
    for(int i=0;i<tmp_inode.i_blocks;i++)
    {
        if(i<6)
            free_block(tmp_inode.i_block[i]);
        else if(i>=6&&i<6+256)
            free_block(get_index_one(i,tmp_inode.i_block[6]));
        else if(i>=6+256)
            free_block(get_index_one(i,tmp_inode.i_block[7]));
    }
    if(tmp_inode.i_blocks>6)
        free_block(tmp_inode.i_block[6]);//删除一级索引块
    if(tmp_inode.i_blocks>6+256)
    {
        for(int j=0;6+256+j*256<tmp_inode.i_blocks;j++)
        {//删除二级索引指向的索引块
            uint8_t tmp_block1[512];
            int tmp_num;
            reload_block_entry(tmp_inode.i_block[7],tmp_block1);
            memcpy(&tmp_num,tmp_block1+j*2,2);
            free_block(tmp_num);
        }
        free_block(tmp_inode.i_block[7]);//删除二级索引块
    }
        
    tmp_inode.i_blocks=0;  
    update_inode_entry(inode_num,&tmp_inode);
}

uint16_t get_inode()//分配一个inode,返回序号 
{
    uint16_t cur=1;
    uint8_t con=128;//0b10000000
    int flag=0;
    if(group_desc.bg_free_inodes_count==0)
    {
        printf("There is no Inode to be alloced!\n");
        return 0;
    }
    reload_inode_bitmap();//将索引节点位图读入缓冲区
    cur=(cur-1)/8;//索引结点位图中相应字节的位置
    while(bitbuf[cur]==255)//寻找可用的字节
    {
        if(cur==511)
        {
            printf("ERROR\n");
            return 0;
        }
        else cur++;
    }
    while(bitbuf[cur]&con)// 寻找位图中的第一个未被占用的位
    {
        con=con/2;//相当于右移一位
        flag++;//标志该字节的第几位，从0开始
    }
    bitbuf[cur]=bitbuf[cur]+con;
    last_alloc_inode=cur*8+flag+1;
    update_inode_bitmap();//进行了索引结点位图的更新
    group_desc.bg_free_inodes_count--;
    update_group_desc();
    return last_alloc_inode;
}
void free_inode(uint16_t inode_num)
{
    reload_inode_bitmap();
    uint8_t con=128;
    int offset;//索引结点在索引结点位图中某一字节的第几位
    int byte_pos=(inode_num-1)/8;//索引结点在索引结点位图中第几个字节中
    for(offset=0;offset<(inode_num-1)%8;offset++)
    {
        con = con >> 1;//右移一位
    }
    if(bitbuf[byte_pos]&con)
    {
        ext2_inode tmp_inode;
        bitbuf[byte_pos]=bitbuf[byte_pos]-con;
        reload_inode_entry(inode_num,&tmp_inode);
        time_t now;
        time(&now);
        tmp_inode.i_dtime=now;
        update_inode_entry(inode_num,&tmp_inode);
        //不必将索引结点清零，修改索引结点位图就代表已释放，这样可以使用删除时间追踪文件系统中的操作
        update_block_bitmap();
        group_desc.bg_free_inodes_count++;
        update_group_desc();
    }
    else{
        printf("the inoded to be freed has not been allocated.\n");
    }
}

void dir_prepare(ext2_inode* inode_i,uint16_t last,uint16_t cur)
{//新建目录时当前目录和上一级目录的初始化
    uint8_t tmp_block[512];
    reload_block_entry(inode_i->i_block[0],tmp_block);
    ext2_dir_entry dir1,dir2;
    memcpy(&dir1,tmp_block,dirsize);
    dir1.inode=cur;
    dir1.rec_len=8;
    inode_i->i_size+=dir1.rec_len;
    dir1.name_len=1;
    dir1.file_type=2;
    strcpy(dir1.name,".");//当前目录
    memcpy(tmp_block,&dir1,dir1.rec_len);
    
    uint16_t offset=dir1.rec_len;
    memcpy(&dir2,tmp_block+offset,dirsize);
    dir2.inode=last;
    dir2.rec_len=9;
    inode_i->i_size+=dir2.rec_len;
    dir2.name_len=2;
    dir2.file_type=2;
    strcpy(dir2.name,"..");//上级目录
    memcpy(tmp_block+offset,&dir2,dir2.rec_len);
    update_block_entry(inode_i->i_block[0],tmp_block);

}

int test_fd(uint16_t inode_num);
void close_file(unsigned char name[255]);
uint16_t search_filename(char tmp[255],uint8_t file_type,ext2_inode* cur_inode,int flag)
{//在当前目录中查找文件或子目录，返回索引节点号,flag=1时删除目录项,flag=2时目录为空才删除
//如果file_type=0，则不确定是查找目录或文件，同名即可
    for(int i=0;i<cur_inode->i_blocks;i++)
    {
        reload_dir_i(i,&dir,0,*cur_inode);
        int offset=0;//读出的目录项的实际长度累加
        while(dir.rec_len!=0)//若读出的目录项长度为0则结束循环
        {
            if(dir.rec_len<0)printf("wrong len\n");
            if(dir.inode)
            {
                if(dir.file_type==file_type||file_type==0)
                {
                    if(strcmp(dir.name,tmp)==0)
                    {
                        ext2_inode tmp_inode;
                        reload_inode_entry(dir.inode,&tmp_inode);
                        if(flag==1||((flag==2)&&(tmp_inode.i_size==17)))
                        {
                            if(file_type==1)
                            {
                                if(test_fd(dir.inode)) 
                                {
                                    int pos;
                                    for(pos=0;pos<16;pos++)
                                    {//关闭文件
                                        if(fopen_table[pos]==dir.inode)
                                        {
                                            fopen_table[pos]=0;
                                            printf("File %s is closed successfully!\n",dir.name);
                                        }
                                    }
                                }
                            }
                            uint16_t num=dir.inode;
                            dir.inode=0;
                            update_dir_i(i,&dir,offset,*cur_inode);
                            cur_inode->i_size-=dir.rec_len;
                            group_desc.bg_used_dirs_count--;
                            update_group_desc();
                            return num;
                        }
                        else
                            return dir.inode;
                    }
                }
            }
            offset+=dir.rec_len;
            if(512-offset<7)break;//此数据块已读完(目录项长度至少7字节)
            reload_dir_i(i,&dir,offset,*cur_inode);
        }
    }
    if(file_type==1) printf("No such file named %s!\n",tmp);
    else if(file_type==2) printf("No such directory named %s!\n",tmp);
    return 0;
}

int test_fd(uint16_t inode_num)
{
    int fopen_table_point;
    for(fopen_table_point=0;fopen_table_point<16;fopen_table_point++)
    {
        if(fopen_table[fopen_table_point]==inode_num)   break;
    }
    if(fopen_table_point==16)return 0;
    return 1;
}

/******************************    初始化    ****************************************/
void initialize_disk() 
{
    printf("Creating the ext2 file system\n");
    last_alloc_inode=1;
    last_alloc_block=0;
    for(int i=0;i<16;i++)fopen_table[i]=0;//清空文件打开表
    // 创建模拟文件系统的虚拟硬盘文件
    time_t now;
    time(&now);
    fp=fopen(PATH, "wb+");
    if (fp == NULL) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    for(uint16_t i=0;i<blocksize;i++)
        block_buffer[i]=0;
    for(uint16_t i=0;i<blocks;i++)
    {
        fseek(fp,DISK_START+i*blocksize,SEEK_SET);
        fwrite(block_buffer,blocksize,1,fp);
    }//清空文件，即清空磁盘全部用0填充

    int a=3;
    fseek(fp,blocksize/2,SEEK_SET);
    fwrite(&a,4,1,fp);

    strcpy(current_path,"[root@ /");  //路径名
    //初始化组描述符
    strcpy(group_desc.bg_volume_name, "EXT2_DISK");
    group_desc.bg_block_bitmap = 1;//保存块位图的块号
    group_desc.bg_inode_bitmap = 2;//保存索引节点位图的块号
    group_desc.bg_inode_table = 3;//索引节点表的起始块号
    group_desc.bg_free_blocks_count=4096;
    group_desc.bg_free_inodes_count=4096;
    group_desc.bg_used_dirs_count=1;
    strcpy(group_desc.psw,"666");
    update_group_desc(); //更新组描述符内容,将组描述符写入文件

    currentdir_inode=get_inode();//分配索引结点并初始化索引结点位图
    reload_inode_entry(currentdir_inode,&inode);
    //初始化索引结点表，添加第一个索引节点
    inode.i_mode =15;// 目录访问权限类型默认rwx
    inode.i_blocks=0;
    inode.i_size = 0; 
    inode.i_atime = now;
    inode.i_ctime = now;
    inode.i_mtime = now;
    inode.i_dtime = 0; // 删除时间为0表示未删除
    inode.i_block[0]=alloc_block();//分配数据块并更新数据块位图
    inode.i_blocks++;

    //创建当前目录和上级目录，并将两个目录项写入虚拟磁盘
    dir_prepare(&inode,currentdir_inode,currentdir_inode);
    update_inode_entry(currentdir_inode,&inode);// 将根目录的索引结点写入虚拟硬盘

    if(fclose(fp))printf("error");
    printf("The ext2 file system has been installed!\n");
}

void initialize_memory() 
{
    last_alloc_inode=1;
    last_alloc_block=0;
    for(int i=0;i<16;i++)fopen_table[i]=0;
    strcpy(current_path,"[root@ /");
    fp=fopen(PATH, "rb");
    
    if(fp==NULL)
    {
        printf("The File system does not exist!\n");
        fclose(fp);
        initialize_disk();
        return ;
    }
    currentdir_inode=1;
    reload_group_desc();
    if(fclose(fp))printf("error");
}

/******************************    命令层    ****************************************/
/*----------   目录   ----------*/
void dir_ls()
{
    fp=fopen(PATH,"rb+");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);
    if(!(cur_inode.i_mode&4)) //检查访问权限
    {
        printf("No permission to list items in current directory!\n");
        return;
    }
    
    printf("mode    type        size      CreateTime                  LastAccessTime              ModifyTime                  name\n");
    time_t now;
    time(&now);
    for(uint16_t i=0;i<cur_inode.i_blocks;i++)
    {
        
        reload_dir_i(i,&dir,0,cur_inode);
        int offset=0;//读出的目录项的实际长度
        while(dir.rec_len!=0)//若读出的目录项长度为0则结束循环
        {
            if(dir.rec_len<0)printf("wrong len\n");
            if(dir.inode)
            {
                
                ext2_inode tmp_inode;
                reload_inode_entry(dir.inode,&tmp_inode);//遍历当前目录下文件的索引结点
                if(tmp_inode.i_mode&4) printf("r_");
                else printf("__");
                if(tmp_inode.i_mode&2) printf("w_");
                else printf("__");
                if(tmp_inode.i_mode&1) printf("x   ");
                else printf("_   ");
                switch (dir.file_type)
                {
                case 1:printf("%-12s","File");printf("%-5uB   ",tmp_inode.i_size);break;
                case 2:printf("%-12s","Directory");printf("%-5uB   ",tmp_inode.i_size);break;
                default:printf("%-12s","others");break;
                }
                
                char timestr[100];
                strcpy(timestr," ");
                strcat(timestr,asctime(localtime(&tmp_inode.i_ctime)));
                strcat(timestr,"   ");
                strcat(timestr,asctime(localtime(&tmp_inode.i_atime)));
                strcat(timestr,"   ");
                strcat(timestr,asctime(localtime(&tmp_inode.i_mtime)));
                strcat(timestr,"   ");
                for(int j=0;j<strlen(timestr);j++)
                    if(timestr[j]=='\n') timestr[j]=' ';
                printf("%s", timestr);
                printf("%-s\n",dir.name);
            }
            offset+=dir.rec_len;
            if(512-offset<7)break;//此数据块已读完,剩下不足7字节为外碎片
            reload_dir_i(i,&dir,offset,cur_inode);
        }
    }
    cur_inode.i_atime=now;
    update_inode_entry(currentdir_inode,&cur_inode);
    fclose(fp);
}

void cd(char path_name[256])
{
    fp=fopen(PATH,"rb");
    uint16_t find_inode;//当前搜索的文件或目录的索引结点
    char search_path[256],new_path[256];//分别用于截断输入的路径和存储新路径
    strcpy(search_path,path_name);
    if(path_name[0]=='/'){//绝对路径
        find_inode=1;//根目录的索引节点号
        for(int i=0;path_name[i];i++)//此时正好能将终止符也复制
            search_path[i]=path_name[i+1];
        strcpy(new_path,"[root@ /");
    }else{//相对路径
        find_inode=currentdir_inode;//当前目录的索引节点号
        strcpy(search_path,path_name);
        strcpy(new_path,current_path);
    }

    char *token;
    // 使用strtok函数分割字符串
    token = strtok(search_path, "/");
    // 循环输出分割后的路径部分
    while (token != NULL) {
        ext2_inode cur_inode;
        reload_inode_entry(find_inode,&cur_inode);
        find_inode=search_filename(token,2,&cur_inode,0);
        ext2_inode tmp_inode;
        reload_inode_entry(find_inode,&tmp_inode);
        if(!(tmp_inode.i_mode&1)) //检查访问权限
        {
            printf("No permission to access to the directory!\n");
            return;
        }
        if(find_inode)
        {
            if(strcmp(path_name,".")==0){
                break;
            }
            
            if(strcmp(path_name,"..")==0)
            {//返回上级目录
                char *lastc = strrchr(new_path, '/');
                if (lastc != NULL) 
                    *lastc = '\0';
                lastc = strrchr(new_path, '/');
                if (lastc != NULL) // 找到最后一个斜杠，截断字符串
                    *lastc = '\0';
                strcat(new_path,"/");
                break;
            }
            //继续按路径寻找目录
            strcat(new_path,dir.name);
            strcat(new_path,"/");
        }
        else{
            fclose(fp);
            return;
        }
        token = strtok(NULL, "/");
    }
    currentdir_inode=find_inode;
    strcpy(current_path,new_path);
    fclose(fp); 
}

void create(uint8_t type_num,char name[255])
{
    fp=fopen(PATH,"rb+");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);
    if((!(cur_inode.i_mode&1))||(!(cur_inode.i_mode&2))) //检查访问权限
    {
        printf("No permission to create file/directory in current directory!\n");
        return;
    }
    if(strlen(name)>255)
        name[255]='\0';
    int new_len=7+strlen(name);
    int flag=0;//是否找到已存在且大小足够的目录项
    ext2_dir_entry tmp_dir;
    uint16_t num;//分配给新文件的目录项在当前目录的第几个数据块号
    int pos=0;//分配给新文件的目录项在数据块中的位置
    for(int i=0;i<cur_inode.i_blocks;i++)
    {
        reload_dir_i(i,&tmp_dir,0,cur_inode);
        int offset=0;//读出的目录项的实际长度累加
        while(tmp_dir.rec_len!=0)//若读出的目录项长度为0则结束循环
        {
            if(tmp_dir.rec_len<0)printf("wrong len");
            if(tmp_dir.inode)
            {
                if(strcmp(tmp_dir.name,name)==0)
                {
                    if(tmp_dir.file_type==1)printf("There has been a file named %s!\n",name);
                    else if(tmp_dir.file_type==2)printf("There has been a directory named %s!\n",name);
                    fclose(fp);
                    return ;
                }
            }
            else//索引结点号为0
            {
                if(tmp_dir.rec_len>=new_len)
                {
                    flag=1;
                    dir=tmp_dir;
                    pos=offset;
                    num=i;
                }
            }
            offset+=tmp_dir.rec_len;
            if(512-offset<7)break;//此数据块已读完(目录项长度至少7字节)
            reload_dir_i(i,&tmp_dir,offset,cur_inode);
        }
        if(tmp_dir.rec_len==0&&flag==0)
        {//从来没有使用过的目录
            if(512-offset>=new_len)
            {//创建新目录项后不会溢出数据块
                flag=1;
                dir=tmp_dir;
                dir.rec_len=7+strlen(name);
                pos=offset;
                num=i;
            }
        }
    }
    if(flag==0)//当前目录的已有数据块不够加入需要创建的目录项
    {//给当前目录分配一个新的数据块并更新当前索引结点的信息
        int block_num=alloc_block();
        if(block_num==-1)   {
            fclose(fp);
            return ;
        }
        num=cur_inode.i_blocks;
        pos=0;
        update_inode_newblock(block_num,&cur_inode);
        update_inode_entry(currentdir_inode,&cur_inode);
        dir.rec_len=7+strlen(name);
    }
    time_t now;
    time(&now);
    dir.file_type=type_num;
    dir.inode=get_inode();
    dir.name_len=strlen(name);
    strcpy(dir.name,name);
    //新建文件/目录的索引结点初始化
    ext2_inode new_inode;
    reload_inode_entry(dir.inode,&new_inode);
    if(type_num==1) new_inode.i_mode =7;// 默认f_r_w_x
    else if(type_num==2) new_inode.i_mode =15;//d_r_w_x
    new_inode.i_blocks=0;
    new_inode.i_size = 0; 
    new_inode.i_atime = now;
    new_inode.i_ctime = now;
    new_inode.i_mtime = now;
    new_inode.i_dtime = 0; // 删除时间为0表示未删除
    new_inode.i_block[0]=alloc_block();//分配数据块并更新数据块位图
    new_inode.i_blocks++;
    if(type_num==2)//目录初始化
        dir_prepare(&new_inode,currentdir_inode,dir.inode);
    update_inode_entry(dir.inode,&new_inode);
    update_dir_i(num,&dir,pos,cur_inode);//在已找到的目录项位置中写入目录项
    cur_inode.i_size += dir.rec_len;
    cur_inode.i_mtime=now;
    update_inode_entry(currentdir_inode,&cur_inode);
    if(type_num==2)//目录初始化
    {
        group_desc.bg_used_dirs_count++;
        update_group_desc();
        printf("Directory %s is created successfully!\n",name);
    }
    else
        printf("File %s is created successfully!\n",name);
    fclose(fp);
}

void delete_file(uint16_t delete_inode_num)
{
    delete_inode_allblock(delete_inode_num);
    free_inode(delete_inode_num);
}
void delete_dir(uint16_t delete_inode_num)
{
    ext2_inode delete_inode;
    reload_inode_entry(delete_inode_num,&delete_inode);
    if(delete_inode.i_size==17)
    {//该目录中没有其他目录或文件
        free_block(delete_inode.i_block[0]);
        free_inode(delete_inode_num);
        group_desc.bg_used_dirs_count--;
        return;
    }
    else{
        ext2_dir_entry tmp_dir;
        for(int i=0;i<delete_inode.i_blocks;i++)
        {
            int offset=17;//读出的目录项的实际长度累加
            reload_dir_i(i,&tmp_dir,17,delete_inode);
            while(tmp_dir.rec_len!=0)//若读出的目录项长度为0则结束循环
            {
                if(tmp_dir.rec_len<0)printf("wrong len\n");
                if(tmp_dir.inode)
                {
                    tmp_dir.inode=0;//其实也可以不用置零，因为之后整个数据块被删除时会全部清零
                    if(tmp_dir.file_type==2)
                    {
                        delete_dir(tmp_dir.inode);
                    }   
                    else if(tmp_dir.file_type==1)
                    {
                        delete_file(tmp_dir.inode);
                    }
                    group_desc.bg_used_dirs_count--;
                }
                offset+=tmp_dir.rec_len;
                if(512-offset<7)break;//此数据块已读完(目录项长度至少7字节)
                reload_dir_i(i,&tmp_dir,offset,delete_inode);
            }
        }
        delete_inode_allblock(delete_inode_num);
        free_inode(delete_inode_num);
    }
    update_group_desc();
}

void delete(uint8_t type_num,char name[255],int flag)
{//注意在search_file中修改当前目录的索引节点大小和修改时间
//flag用于判断是否需要递归删除,删除文件时的flag为0便于search_file中修改索引结点
    fp=fopen(PATH,"rb+");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);
    if((!(cur_inode.i_mode&1))||(!(cur_inode.i_mode&2))) //检查访问权限
    {
        printf("No permission to delete file/directory in current directory!\n");
        return;
    }
    //如果找到了直接在调用的函数中将目录项的索引节点号赋值为0,即删除目录项
    uint16_t delete_inode_num=search_filename(name,type_num,&cur_inode,1+flag);
    
    if(delete_inode_num)
    {
        ext2_inode delete_inode;
        if(type_num==2)//目录
        {
            if(flag==0)//递归删除目录
                delete_dir(delete_inode_num);
            else{//只删除空目录
                reload_inode_entry(delete_inode_num,&delete_inode);
                if(delete_inode.i_size==17)
                {
                    free_block(delete_inode.i_block[0]);
                    free_inode(delete_inode_num);
                }
                else
                {
                    printf("The block is not empty.\n");
                    fclose(fp);
                    return;
                }
            }
        }
        else if(type_num==1)
        {
            delete_file(delete_inode_num);
        }
        time_t now;
        time(&now);
        cur_inode.i_mtime=now;
        update_inode_entry(currentdir_inode,&cur_inode);
    }
    else
    {
        fclose(fp);
        return;
    }
    printf("%s is deleted successfully!\n",name);
    fclose(fp);
}

/*----------   文件   ----------*/
void open_file(unsigned char name[255])
{
    fp=fopen(PATH,"rb");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);//获取当前目录的索引结点
    uint16_t inode_num=search_filename(name,1,&cur_inode,0);
    if(inode_num)
    {
        if(test_fd(inode_num))
            printf("File %s has been opened!\n",name);
        else
        {
            int pos;
            for(pos=0;pos<16;pos++)
            {
                if(fopen_table[pos]==0)
                {
                    fopen_table[pos]=inode_num;
                    printf("File %s is opened successfully!\n",name);
                    return;
                }
            }
            if(pos==16) printf("Fopen_table is full!\n");
        }
    }
}
void close_file(unsigned char name[255])
{
    fp=fopen(PATH,"rb");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);//获取当前目录的索引结点
    uint16_t inode_num=search_filename(name,1,&cur_inode,0);
    if(inode_num)
    {
        if(test_fd(inode_num))
        {
            int pos;
            for(pos=0;pos<16;pos++)
            {
                if(fopen_table[pos]==inode_num)
                {
                    fopen_table[pos]=0;
                    printf("File %s is closed successfully!\n",name);
                }
            }
        }
        else printf("File %s has not been opened!\n",name);
    }
    fclose(fp);
}

void read_file(char name[512])
{
    fp=fopen(PATH,"rb+");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);//获取当前目录的索引结点
    uint16_t inode_num=search_filename(name,1,&cur_inode,0);
    if(inode_num)
    {
        if(test_fd(inode_num))
        {
            ext2_inode read_inode;
            reload_inode_entry(inode_num,&read_inode);
            if(!(read_inode.i_mode&4))//111b:读,写,执行
            {
                printf("No permission to read file %s !\n",name);
                return;
            }
            int i;
            int a=read_inode.i_blocks;
            printf("\n");
            for(i=0;i<read_inode.i_blocks;i++)
            {
                char tmp_block[512];
                reload_block_i(i,tmp_block,read_inode);
                printf("%s",tmp_block);
            }
            if(read_inode.i_size==0) printf("File %s is empty!\n",name);
            else printf("\n");
            printf("\n");
            time_t now;
            time(&now);
            read_inode.i_atime=now;
            update_inode_entry(inode_num,&read_inode);
        }
        else    printf("File %s has not been opened!\n",name);
    }
    fclose(fp);
}
void write_file(unsigned char name[512],int flag,char source[2560])
{//写入文件覆盖原本的内容或者在文件后面追加内容,用flag区分写入方式（和linux的echo命令类似）
    fp=fopen(PATH,"rb+");
    ext2_inode cur_inode;
    uint32_t size=strlen(source);
    //printf("size%d\n",size);
    reload_inode_entry(currentdir_inode,&cur_inode);//获取当前目录的索引结点
    uint16_t inode_num=search_filename(name,1,&cur_inode,0);
    if(!test_fd(inode_num))
    {
        printf("File %s has not been opened!\n",name);
        fclose(fp);
        return;
    }
    if(inode_num&&test_fd(inode_num))
    {
        ext2_inode write_inode;
        reload_inode_entry(inode_num,&write_inode);
        if(!(write_inode.i_mode&2))//111b:读,写,执行
        {
            printf("No permission to write into file %s !\n",name);
            fclose(fp);
            return;
        }
        char tmp_block[512];//数据块缓冲区
        if(flag==0)//写入文件覆盖原本的内容
        {
            if(size>blocksize*(write_inode.i_blocks+group_desc.bg_free_blocks_count))
            {//判断空闲空间是否足够
                printf("No enough space!\n");
                fclose(fp);
                return;
            }
            delete_inode_allblock(inode_num);
            reload_inode_entry(inode_num,&write_inode);
            while((strlen(source)>512))
            {
                int block_num=alloc_block();
                update_inode_newblock(block_num,&write_inode);
                update_inode_entry(inode_num,&write_inode);
                strncpy(tmp_block, source, 512);
                source = source + 512;
                update_block_entry(block_num,tmp_block);
            }
            //需要分配的空间<=512时分配最后一块
            int block_num=alloc_block();
            update_inode_newblock(block_num,&write_inode);
            update_inode_entry(inode_num,&write_inode);
            strcpy(tmp_block, source);
            update_block_entry(block_num,tmp_block);
            write_inode.i_size=size;
        }
        else if(flag==1)//在文件后面追加内容
        {
            if(size>write_inode.i_blocks*blocksize-write_inode.i_size+blocksize*group_desc.bg_free_blocks_count)
            {//可使用空间为文件原有剩余空间+可分配空间
                printf("No enough space!\n");
                fclose(fp);
                return;
            }
            if(size<=write_inode.i_blocks*blocksize-write_inode.i_size)
            {//无需额外分配数据块，直接写入文件最后一块
                reload_block_entry(write_inode.i_block[write_inode.i_blocks-1],tmp_block);
                strcat(tmp_block,source);
                update_block_entry(write_inode.i_block[write_inode.i_blocks-1],tmp_block);
            }
            else
            {//先填满文件最后一块，再分配新的空间
                reload_block_entry(write_inode.i_block[write_inode.i_blocks-1],tmp_block);
                strncat(tmp_block,source,write_inode.i_blocks*blocksize-write_inode.i_size);
                update_block_entry(write_inode.i_block[write_inode.i_blocks-1],tmp_block);
                source=source+write_inode.i_blocks*blocksize-write_inode.i_size;
                while((strlen(source)>512))
                {
                    int block_num=alloc_block();
                    update_inode_newblock(block_num,&write_inode);
                    update_inode_entry(inode_num,&write_inode);
                    strncpy(tmp_block, source, 512);
                    source = source + 512;
                    update_block_entry(block_num,tmp_block);
                }
                //需要分配的空间<=512时分配最后一块
                int block_num=alloc_block();
                update_inode_newblock(block_num,&write_inode);
                update_inode_entry(inode_num,&write_inode);
                strcpy(tmp_block, source);
                update_block_entry(block_num,tmp_block);
            }
            write_inode.i_size+=size;
        }
        time_t now;
        time(&now);
        write_inode.i_mtime=now;
        update_inode_entry(inode_num,&write_inode);
    }
    fclose(fp);
}

void attrib(unsigned char name[255],char rwx[6])
{//修改当前目录下某个文件或目录的访问权限
    fp=fopen(PATH,"rb+");
    ext2_inode cur_inode;
    reload_inode_entry(currentdir_inode,&cur_inode);
    uint16_t inode_num=search_filename(name,0,&cur_inode,0);
    if(!inode_num) return;

    char passw[16];
    printf("Please input the password: ");
    scanf("%s",passw);
    if(strcmp(passw,group_desc.psw))
    {
        printf("Wrong password!\n");
        return;
    }

    ext2_inode ch_inode;
    reload_inode_entry(inode_num,&ch_inode);
    ch_inode.i_mode=0;
    if(strchr(rwx, 'r') != NULL) ch_inode.i_mode+=4;
    if(strchr(rwx, 'w') != NULL) ch_inode.i_mode+=2;
    if(strchr(rwx, 'x') != NULL) ch_inode.i_mode+=1;
    time_t now;
    time(&now);
    ch_inode.i_mtime=now;
    update_inode_entry(inode_num,&ch_inode);
    
    fclose(fp);
}


void help()
{
    printf("   *************************************************************************************\n");
    printf("   *                   An simulation of ext2 file system                               *\n");
    printf("   *                                                                                   *\n");
    printf("   * The available commands are:                                                       *\n");
    printf("   * 1.change dir   : cd+dir_name/path     2.create dir       : mkdir+dir_name         *\n");
    printf("   * 3.create file  : mkf+file_name        4.delete empty dir : rmdir+dir_name         *\n");
    printf("   * 5.delete file  : rm+file_name         6.delete all in dir: rmdir+-r+dir_name      *\n");
    printf("   * 7.open   file  : open+file_name       8.write  file      : write+file_name+>/>>+  *\n");
    printf("   * 9.close  file  : close+file_name      10.read  file      : read+file_name         *\n");
    printf("   * 11.list  items : ls                   12.this  menu      : help                   *\n");
    printf("   * 13.format disk : format               14.password        : password               *\n");
    printf("   * 15.login       : login                16.logoff          : logoff                 *\n");
    printf("   * 17.change imode: chmod+name+rwx       18.exit            : quit                   *\n");
    printf("   *************************************************************************************\n");
}

/******************************    用户接口层    ****************************************/
void format()
{//格式化，会清空磁盘数据
    char sign[3];
    while(1)
    {
        printf("Sure to format the file system? [Y/n]: ");
        scanf("%s",sign);
        if(sign[0]=='N'||sign[0]=='n') break;
        else if(sign[0]=='Y'||sign[0]=='y')
        {
            initialize_disk();
            initialize_memory();
            break;
        }
        else printf("Please input [Y/n]!\n");
    }
}
int password()
{//修改密码，修改成功返回1
    char passw[16];
    printf("Please input the old password: ");
    scanf("%s",passw);
    if(strcmp(passw,group_desc.psw))
    {
        printf("Wrong password!\n");
        return 0;
    }
    printf("Please input the new password: ");
    scanf("%s",passw);
    strcpy(group_desc.psw,passw);
    fp=fopen(PATH,"rb+");
    update_group_desc();
    reload_group_desc();
    fclose(fp);
    return 1;
}
void quit()
{
    printf("Thank you for using! Bye~\n");
}
int login()
{
    char passw[16];
    fp=fopen(PATH,"rb");
    reload_group_desc();
    fclose(fp);
    printf("Please input the password: ");
    scanf("%s",passw);
    
    if(!strcmp(group_desc.psw,passw))    return 1;
    else{
        printf("Wrong password! Please try again: ");
        scanf("%s",passw);
        if(!strcmp(group_desc.psw,passw))    return 1;
        else quit();
    }
    return 0;
}
void logoff()
{
    char sign[3];
    while(1)
    {
        printf("Sure to log out? [Y/n]: ");
        scanf("%s",sign);
        if(sign[0]=='N'||sign[0]=='n') break;
        else if(sign[0]=='Y'||sign[0]=='y')
        {
            initialize_memory();
            printf("[$$$]# ");
            char command_[6];
            scanf("%s",command_);
            if(!strcmp(command_,"login"))
            {
                if(login()) break;
                else return;
            }
            else if(!strcmp(command_,"quit"))
            {
                quit();
                return;
            }
                
        }
        else printf("Please input [Y/n]!\n");
    }
}

void shell()
{
    char command[10];
    ext2_inode tmp_inode;
    while(1)
    {
        printf("%s]# ",current_path);
        scanf("%s",command);
        char name[255];
        if(!strcmp(command,"cd"))//change dir
        {
            char path[256];
            scanf("%s",path);
            cd(path);
        }
        else if(!strcmp(command,"mkdir"))//create dir
        {
            scanf("%s",name);
            create(2,name);
        }
        else if(!strcmp(command,"mkf"))//create file
        {
            scanf("%s",name);
            create(1,name);
        }
        else if(!strcmp(command,"rmdir"))
        {
            scanf("%s",name);
            if(!strcmp(name,"-r"))//delete all in dir
            {
                char name_[255];
                scanf("%s",name_);
                delete(2,name_,0);
            }
            else 
            {
                delete(2,name,1);//delete empty dir
            }
        }
        else if(!strcmp(command,"rmf"))//delete file
        {
            scanf("%s",name);
            delete(1,name,0);
        }
        else if(!strcmp(command,"open"))
        {
            scanf("%s",name);
            open_file(name);
        }
        else if(!strcmp(command,"close"))
        {
            scanf("%s",name);
            close_file(name);
        }
        else if(!strcmp(command,"read"))
        {
            scanf("%s",name);
            read_file(name);
        }
        else if(!strcmp(command,"write"))
        {
            scanf("%s",name);
            char flag[3];
            scanf("%s",flag);
            char source[2560];
            scanf("%s",source);
            if(!strcmp(flag,">"))
                write_file(name,0,source);
            else if(!strcmp(flag,">>"))
                write_file(name,1,source);
            else
                printf("Wrong command!\n"); 
        }
        else if(!strcmp(command,"ls"))//list   items
            dir_ls();
        else if(!strcmp(command,"help"))//this  menu
            help();
        else if(!strcmp(command,"format"))//format disk
        {
            format();
        }
        else if(!strcmp(command,"password"))
        {
            password();
        }
        else if(!strcmp(command,"login"))
        {
            printf("Failed! You haven't logged out yet.\n");
        }
        else if(!strcmp(command,"logoff"))
        {
            logoff();
        }
        else if(!strcmp(command,"chmod"))
        {
            scanf("%s",name);
            char rwx[6];
            scanf("%s",rwx);
            attrib(name,rwx);
        }
        else if(!strcmp(command,"quit"))//logoff
        {
            quit();
            break;
        }
        else
            printf("No this Command.Please check!\n");
    }
}

int main()
{
    initialize_disk();
    initialize_memory();
    printf("Hello! Welcome to simulation of ext2 file system!\n");
    if(!login()) return 0;
    shell();
    return 0;
}

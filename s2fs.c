#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/pagemap.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

/* [X1: point 1]
 * Specify copywrite information for the kernel module
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("James Heiman");
MODULE_DESCRIPTION("LKP Project 4");

#define S2FS_MAGIC_NUMBER 0x13131314

//prototypes for filling out structs
static struct dentry *s2fs_mount(struct file_system_type *type, int flags, char const *dev, void *data);
static struct inode *s2fs_make_inode(struct super_block *sb, int mode);
static struct dentry *s2fs_create_dir(struct super_block *sb, struct dentry *parent, const char *dir_name);
static struct dentry *s2fs_create_file(struct super_block *sb, struct dentry *dir, const char *file_name, int pid);

//struct to hold fs params
static struct file_system_type s2fs_type = {
  .owner = THIS_MODULE,
  .name = "s2fs",
  .mount = s2fs_mount,
  .kill_sb = kill_litter_super,
};

/*********************/
/* PROCESS FUNCTIONs */
/*********************/


struct task_struct *find_root(void)
{
  struct task_struct* root = current;
  while(root->pid != 0)
  {
    root = root->parent;
  }
  return root;
}

static void print_process(struct task_struct *task, struct super_block *sb, struct dentry *parent)
{
    
    struct list_head* list;   
    struct task_struct *cur;
    char buff[256];
    
    if(task->pid == 0)
    {
      sprintf(buff, "%d", task->pid);
      parent = s2fs_create_dir(sb, parent, buff);
      s2fs_create_file(sb, parent, buff, task->pid);
    }
    
    list_for_each(list, &task->children)
    {
      cur = list_entry(list, struct task_struct, sibling);
      sprintf(buff, "%d", cur->pid);
      if(!list_empty(&cur->children))
      {
        struct dentry* new_parent = s2fs_create_dir(sb, parent, buff);
        s2fs_create_file(sb, new_parent, buff, cur->pid);
        print_process(cur, sb, new_parent); //create directory and recursively call print_process
      }
      else
      {
        s2fs_create_file(sb, parent, buff, cur->pid);
      }
    }
    
}

/*********************/
/* FS CREATION FUNCS */
/*********************/

static void s2fs_put_super(struct super_block *sb)
{
    printk("s2fs super block destroyed\n");
}

static struct super_operations const s2fs_super_ops = {
    .put_super = s2fs_put_super,
};


//function to create an inode, takes the superblock of the filesystem
//Mode determines if inode is file or directory and permission bits
static struct inode *s2fs_make_inode(struct super_block *sb, int mode)
{
  struct inode *entry = NULL;
  
  entry = new_inode(sb);
  
  if(entry)
  {
    entry->i_ino = get_next_ino();
    entry->i_mode = mode;
    entry->i_uid = KUIDT_INIT(0);
    entry->i_gid = KGIDT_INIT(0);
    entry->i_blocks = 0;
    entry->i_atime = entry->i_mtime = entry->i_ctime = current_time(entry);
  }
  
  return entry;
}

/*******************/
/* FILE OPERATIONS */
/*******************/

//function to open a file, copies over file properties to inode properties
static int s2fs_open(struct inode *ino, struct file *fp)
{
  fp->private_data = ino->i_private;
  return 0;
}

//function to get task info and print it to a buffer
int get_task_info(int pid, char* data)
{
  struct pid *pidp = find_get_pid( (pid_t) pid );
  struct task_struct* task = get_pid_task(pidp, PIDTYPE_PID);
  if(task == NULL)
    return -1;
  sprintf(data, "Name: %s\nState: %ld\nPID: %d\nCPU_ID: %d\n\
TGID: %d\nPPID: %d\nStart_Time: %llu\n\
Dynamic_Priority: %d\nStatic_Priority: %d\n\
Normal_Priority: %d\nReal_Time_Priority: %d\n\
Memory_Map_Base: %lu\nVirtual_Memory_Space: %lu\n\
Virtual_Memory_Usage: %lu\nVirtual_Memory_Num: %d\nPages_Mapped: %lu\n", 
          task->comm, task->state, task->pid, task->cpu, task->tgid, task->real_parent->pid,
          task->start_time, task->prio, task->static_prio, task->normal_prio, task->rt_priority, 
          task->mm->mmap_base, task->mm->task_size, task->mm->highest_vm_end, task->mm->map_count, task->mm->total_vm); 
  return strlen(data);
}


//function to get a pid from a file pointer
int get_pid_from_file(struct file *fp) // 
{
  int pid;
  char *file_name = fp->f_path.dentry->d_iname;
  sscanf(file_name, "%d", &pid);
  
  return pid;
}

#define TMP_SIZE 1024
//function to read a file
static ssize_t s2fs_read_file(struct file *fp, char *buf, size_t count, loff_t *offset)
{
  int len;
  char tmp[TMP_SIZE];
  int pid = get_pid_from_file(fp);
  len = get_task_info(pid, tmp);
  if(len == -1)
    len = snprintf(tmp, TMP_SIZE, "Task no longer exists.\n");
  
  if(*offset > len)
    return 0;
  if(count > len - *offset)
    count = len - *offset;
  
  //get data and update counter
  if(copy_to_user(buf, tmp, count))
    return -EFAULT;
  
  //update offset and return  
  *offset += count;
  return count;
}

//function to write to file
static ssize_t s2fs_write_file(struct file *fp, const char *buf, size_t count, loff_t *offset)
{
  atomic_t *counter = (atomic_t *) fp->private_data;
  char tmp[TMP_SIZE];
  
  //Write only from the beginning
  if(*offset != 0)
    return -EINVAL;
  
  //Read from user
  if(count >= TMP_SIZE)
    return -EINVAL;
  memset(tmp, 0, TMP_SIZE);
  if(copy_from_user(tmp, buf, count))
    return -EFAULT;
  
  //update counter
  atomic_set(counter, simple_strtol(tmp, NULL, 10));
  return count;
  
  
}

//set file operations to use the above functions
static struct file_operations s2fs_fops = {
  .open = s2fs_open,
  .read = s2fs_read_file,
  .write = s2fs_write_file,
};

//function to create a file dentry and inode
static struct dentry *s2fs_create_file(struct super_block *sb, struct dentry *dir, const char *file_name, int pid)
{
  struct dentry *ret;
  struct inode *ino;
  struct qstr qname;
  
  //set file name properties
  qname.name = file_name;
  qname.len = strlen(file_name);
  qname.hash = full_name_hash(dir, file_name, qname.len);
  //allocate dentry with the file name
  ret = d_alloc(dir, &qname);
  if(!ret)
    return 0;
  //allocate inode for file 
  ino = s2fs_make_inode(sb, S_IFREG | 0644);
  if(!ino)
  {
    dput(ret);
    return 0;
  }
  //set file operations on the inode
  ino->i_fop = &s2fs_fops;
  ino->i_private = &pid;
  //ino->i_private
  //add the dentry and ino pair
  d_add(ret, ino);
  return ret;
}

//function to create a directory dentry and inode
static struct dentry *s2fs_create_dir(struct super_block *sb, struct dentry *parent, const char *dir_name)
{
  struct dentry *ret = NULL;
  struct inode *ino = NULL;
  struct qstr qname;
  
  //fill the directory name parameters
  qname.name = dir_name;
  qname.len = strlen(dir_name);
  qname.hash = full_name_hash(parent, dir_name, qname.len);
  ret = d_alloc(parent, &qname);
  if(!ret)
    return 0;
  
  ino = s2fs_make_inode(sb, S_IFDIR | 0755);
  if(!ino)
  {  
    dput(ret);
    return 0;
  }
  
  //set inode operations
  ino->i_op = &simple_dir_inode_operations;
  ino->i_fop = &simple_dir_operations;
  
  d_add(ret, ino);
  
  return ret;
}

//function for creating the sb needed for mounting the filesystem
static int s2fs_fill_sb(struct super_block *sb, void *data, int silent)
{
  struct inode *root = NULL;
  struct dentry *root_dentry;
  
  //set superblock params
  sb->s_blocksize = VMACACHE_SIZE;
  sb->s_blocksize_bits = L1_CACHE_SHIFT;
  sb->s_magic = S2FS_MAGIC_NUMBER;
  sb->s_op = &s2fs_super_ops;
  
  
  //make inode for root directory
  root = s2fs_make_inode(sb, S_IFDIR | 0755);
  if (!root)
  {
       printk("inode allocation failed\n");
       return -ENOMEM;
  }
  
  root->i_op = &simple_dir_inode_operations;
  root->i_fop = &simple_dir_operations;
  
  //create and set root dentry for the superblock
  root_dentry = d_make_root(root);
  if(!root_dentry)
  {
    iput(root);
    return -ENOMEM;
  }    
  sb->s_root = root_dentry;
  
  print_process(find_root(), sb, root_dentry);
  
  return 0;
}

//function for defining how to mount the filesystem
static struct dentry *s2fs_mount(struct file_system_type *type, int flags, char const *dev, void *data)
{
  struct dentry *const entry = mount_nodev(type, flags, data, s2fs_fill_sb);
  if (IS_ERR(entry))
      printk("s2fs mounting failed\n");
  else
      printk("s2fs mounted\n");
  return entry;
}

static void cleanup(void)
{
	unregister_filesystem(&s2fs_type);
}

static int __init s2fs_init(void)
{
	int err = 0;
  register_filesystem(&s2fs_type);
	if (err)
		goto out;
  
out:
	
	return err;
}

static void __exit s2fs_exit(void)
{

	cleanup();
	return;
}

/* [X21: point 1]
 * Define the module init point
 */
module_init(s2fs_init);

/* [X22: point 1]
 * Define the module exit point
 */
module_exit(s2fs_exit);

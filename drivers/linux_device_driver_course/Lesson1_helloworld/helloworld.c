#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/module.h>

/*
** Module Init function
*/
static int __init hello_world_init(void)
{
    printk(KERN_INFO "This is the Simple Module\n");
    return 0;
}
/*
** Module Exit function
*/
static void __exit hello_world_exit(void)
{
    printk(KERN_INFO "Kernel Module Removed Successfully...Bye\n");
}

module_init(hello_world_init);
module_exit(hello_world_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tuyen Hoang Van <tuyenlua9x@gmail.com>");
MODULE_DESCRIPTION("A simple hello world driver");
MODULE_VERSION("1.0.0");

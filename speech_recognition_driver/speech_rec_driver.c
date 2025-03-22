#include<linux/cdev.h>
#include<linux/fs.h>
#include<linux/init.h>
#include<linux/module.h>
#include<linux/slab.h>
#include<linux/uaccess.h>
#include<linux/version.h>
#include<linux/kernel.h>
#include<linux/err.h>
#include<linux/mutex.h>
#include<linux/sched.h>
#include<linux/workqueue.h>
#include<asm/errno.h>

#include "structs.h"

#define DRIVER_NAME	"speech_driver"
#define ERROR		-1
#define SUCCESS	0

#define MAX_NEG_INT32	((int) 0x80000000)

#define SCALE		1000000LL

#define MFCCMUL(a,b)    ((a)*(b))
#define GMMSUB(a,b)	((a)-(b))

#define COMPUTE_GMM_MAP(_idx)                           \
    diff[_idx] = (int64_t)(obs[_idx] - mean[_idx]);                \
    sqdiff[_idx] = MFCCMUL(diff[_idx], diff[_idx]);     \
    compl[_idx] = MFCCMUL((uint64_t)sqdiff[_idx], ((uint64_t)var[_idx])) / SCALE;
#define COMPUTE_GMM_REDUCE(_idx)                \
    d = GMMSUB(d, compl[_idx]);
    
static DEFINE_MUTEX(speech_mutex);

static void insertion_sort_cb(ptm_topn_t **cur, ptm_topn_t *worst, ptm_topn_t *best, int32_t cw, int32_t intd);
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static void workqueue_handler_eval_cb(struct work_struct* work);


static dev_t dev_id = 0;
static int alloc_ret = -1;
static int cdev_ret = -1;
static struct class* speech_class;
static struct device* speech_device;
static struct cdev* speech_cdev;
static struct workqueue_struct* speech_workqueue;
static struct work_struct speech_work;

static speech_struct* speech_str;


static void insertion_sort_cb(ptm_topn_t **cur, ptm_topn_t *worst, ptm_topn_t *best, int32_t cw, int32_t intd)
{
    for (*cur = worst - 1; *cur >= best && intd >= (*cur)->score; --*cur)
        memcpy(*cur + 1, *cur, sizeof(**cur));
    ++*cur;
    (*cur)->cw = cw;
    (*cur)->score = intd;
}
static int device_open(struct inode* inode, struct file* file)
{
  	pr_info("Opened file\n");
  	return SUCCESS;
}
static int device_release(struct inode* inode, struct file* file)
{
  	pr_info("Closed file\n");
  	return SUCCESS;
}
static ssize_t device_write(struct file* file, const char __user * buffer, size_t length, loff_t* offset)
{
    int retVal = length;

      	//mutex lock
      	mutex_lock(&speech_mutex);
		if(copy_from_user(speech_str,buffer,length))
		{
			retVal = -EFAULT;
			pr_info("Error writing\n");	
		}
		else
		{
			queue_work(speech_workqueue,&speech_work);
		}
	mutex_unlock(&speech_mutex);
	//mutex unlock
      
    return retVal;
}
static ssize_t device_read(struct file* file, char __user * buffer, size_t length, loff_t* offset)
{
	ssize_t bytes_to_read = sizeof(speech_struct);
	
	if(*offset >= bytes_to_read)
	{
		*offset = 0;
		return SUCCESS;
	}
	if(copy_to_user(buffer,speech_str,bytes_to_read))
	{
		return -EFAULT;
		pr_info("Error reading\n");	
	}

	*offset = (*offset) + bytes_to_read;

	return bytes_to_read;
}
static void workqueue_handler_eval_cb(struct work_struct* work)
{
 ptm_topn_t *worst, *best, *topn;
    int32_t *mean, *var, *det, *detP, *detE;
    int16_t i;
    int32_t ceplen;
    int16_t max;
    
 mutex_lock(&speech_mutex);
 
 	if(speech_str->ready == false)
 	{
		    max = speech_str->max;
		    best = topn = &(speech_str->topn[0]);
		    worst = topn + (max - 1);
		    mean = &(speech_str->mean[0]);
		    var = &(speech_str->var[0]);
		    det = &(speech_str->det[0]);
		    detE = det + speech_str->density;
		    ceplen = speech_str->featlen;

		    for (detP = det; detP < detE; ++detP) {
			int64_t diff[4], sqdiff[4];
			uint64_t compl[4]; /* diff, diff^2, component likelihood */
			int64_t d, thresh;
			int32_t d32;
			int32_t *obs;
			ptm_topn_t *cur;
			int32_t cw, j, mod;
			
			d = ((int64_t)(*detP)) * SCALE;
			thresh = ((int64_t)(worst->score)) * SCALE; /* Avoid int-to-float conversions */
			obs = &(speech_str->z[0]);
			cw = detP - det;
			mod = ceplen % 4;
			/* Unroll the loop starting with the first dimension(s).  In
			 * theory this might be a bit faster if this Gaussian gets
			 * "knocked out" by C0. In practice not. */
			for (j = 0; (j < mod) && (d >= thresh); ++j) {
			    diff[0] = (int64_t)(*obs++ - *mean++);
			    sqdiff[0] = MFCCMUL(diff[0], diff[0]);
			    compl[0] = MFCCMUL((uint64_t)sqdiff[0], (uint64_t)(*var++)) / SCALE;
			    d = GMMSUB(d, compl[0]);	    
			}
			/* Now do 4 dimensions at a time.  You'd think that GCC would
			 * vectorize this?  Apparently not.  And it's right, because
			 * that won't make this any faster, at least on x86-64. */
			for (; j < ceplen && d >= thresh; j += 4) {
			    COMPUTE_GMM_MAP(0);
			    COMPUTE_GMM_MAP(1);
			    COMPUTE_GMM_MAP(2);
			    COMPUTE_GMM_MAP(3);
			    COMPUTE_GMM_REDUCE(0);
			    COMPUTE_GMM_REDUCE(1);
			    COMPUTE_GMM_REDUCE(2);
			    COMPUTE_GMM_REDUCE(3);
			
			    var += 4;
			    obs += 4;
			    mean += 4;
			}
			if (j < ceplen) {
			    /* terminated early, so not in topn */
			    mean += (ceplen - j);
			    var += (ceplen - j);
			    continue;
			}
			if (d < thresh)
			    continue;
			    
			for (i = 0; i < max; i++) {
			    if (topn[i].cw == cw)
				break;
			}
			if (i < max){
			    continue;       /* already there.  Don't insert */
			}
	
			d32 = (int32_t)(d / SCALE);
			if (d32 < MAX_NEG_INT32)  
			    insertion_sort_cb(&cur, worst, best, cw, MAX_NEG_INT32);
			else
			    insertion_sort_cb(&cur, worst, best, cw, d32);
			}
	speech_str->ready = true;
 	}
 mutex_unlock(&speech_mutex);
}
static struct file_operations chardev_fops =
{
	.owner = THIS_MODULE,
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release,
};
static int __init speech_driver_init(void)
{
  int retVal = SUCCESS;
  
  alloc_ret = alloc_chrdev_region(&dev_id, 0, 1, DRIVER_NAME); 
  if(alloc_ret)
  {
  	pr_err("Error alloc\n");
  	retVal = ERROR;
  	goto done;
  }

  speech_class = class_create(THIS_MODULE,"speech_class");
  if(speech_class == NULL)
  {
  	pr_err("Error class\n");
  	goto error_reg;
  }
  speech_device = device_create(speech_class,NULL,dev_id,NULL,"speech_device");
  
  if(speech_device == NULL)
  {
  	pr_err("Error device\n");
  	goto error_class;
  }
  
  speech_cdev = cdev_alloc();
  speech_cdev->ops = &chardev_fops;
  speech_cdev->owner = THIS_MODULE;
  
  cdev_ret = cdev_add(speech_cdev,dev_id,1);
  
  if(cdev_ret)
  {
  	pr_err("Error cdev\n");
  	goto error_device;	
  }
  speech_str = (speech_struct*)kzalloc(sizeof(speech_struct),GFP_KERNEL);
  
  if(speech_str == NULL)
  {
   	pr_err("Error kzalloc\n");
   	goto error_cdev;
  }
  
  speech_workqueue = alloc_workqueue("speech_workqueue",WQ_UNBOUND,1);
  if(speech_workqueue == NULL)
  {
    pr_err("Error work\n");
    goto error_workqueue;
  }
  INIT_WORK(&speech_work,workqueue_handler_eval_cb);
  
  pr_info("Successfully created driver!");

  goto done;
  
  error_workqueue:
  	kfree(speech_str);
  	
  error_cdev:
  	cdev_del(speech_cdev);
  
  error_device:
  	device_destroy(speech_class,dev_id);
  	
  error_class:
  	class_destroy(speech_class);	
  		
  error_reg:
  	unregister_chrdev_region(dev_id,1);
  	retVal = ERROR;
  	
  done:
  return retVal;
  
}
static void __exit speech_driver_exit(void)
{
	destroy_workqueue(speech_workqueue);
	kfree(speech_str);
	cdev_del(speech_cdev);
   	device_destroy(speech_class,dev_id);
   	class_destroy(speech_class);
   	unregister_chrdev_region(dev_id,1);
 
   	pr_info("Deleted driver\n");
}
module_init(speech_driver_init);
module_exit(speech_driver_exit);

MODULE_LICENSE("GPL");

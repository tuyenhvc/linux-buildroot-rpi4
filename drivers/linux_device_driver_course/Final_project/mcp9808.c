#include <linux/module.h>      // Thư viện cơ bản nhất để viết Kernel Module
#include <linux/i2c.h>         // Thư viện cung cấp các API giao tiếp với I2C Bus
#include <linux/fs.h>          // Thư viện xử lý File System (dùng để xin cấp phát Major/Minor Number)
#include <linux/cdev.h>        // Thư viện hỗ trợ tạo và quản lý Character Device
#include <linux/uaccess.h>     // Thư viện cung cấp hàm copy dữ liệu giữa Kernel Space và User Space
#include <linux/device.h>      // Thư viện hỗ trợ tạo Device Node (các file ảo trong thư mục /dev/)
#include <linux/slab.h>        // Thư viện cung cấp hàm cấp phát bộ nhớ động trong Kernel (kzalloc)
#include <linux/version.h>     // Thư viện chứa macro kiểm tra phiên bản Kernel đang biên dịch

#define DRIVER_NAME "mcp9808_custom" // Tên của Driver sẽ xuất hiện trong danh sách các driver của hệ thống
#define CLASS_NAME  "mcp9808_class"  // Tên của thư mục Class ảo sẽ được tạo ra trong /sys/class/

#define MCP9808_REG_CONFIG    0x01   // Địa chỉ thanh ghi Cấu hình (dùng để ra lệnh ngủ/thức)
#define MCP9808_REG_TEMP      0x05   // Địa chỉ thanh ghi chứa dữ liệu Nhiệt độ
#define MCP9808_REG_MANUF_ID  0x06   // Địa chỉ thanh ghi chứa ID của nhà sản xuất (Để xác thực IC)

static dev_t mcp9808_base_devt;            // Biến toàn cục (dev_t) lưu trữ chung Major và Minor Base
static struct class *mcp9808_class = NULL; // Con trỏ lưu trữ Class, dùng chung cho tất cả các con cảm biến
static int minor_count = 0;                // Biến đếm số lượng cảm biến đang cắm để tự động tăng Minor ID

// Cấu trúc dữ liệu đại diện cho một con cảm biến mcp9808 cụ thể
struct mcp9808_dev {
    struct i2c_client *client;       // Con trỏ chứa thông tin phần cứng I2C (địa chỉ slave, adapter...)
    int minor_id;                    // Số thứ tự Minor độc lập của riêng cảm biến này (0, 1, 2...)
    struct cdev cdev;                // Đối tượng Character Device quản lý các thao tác File của cảm biến này
    struct device *mcp_device;       // Con trỏ giữ thông tin của file thiết bị vừa tạo trong /dev/
};

// Hàm này được kích hoạt khi user-space chạy lệnh `cat` hoặc hàm `open()` lên file
static int mcp9808_open(struct inode *inode, struct file *file) 
{
    // Tìm ngược lại con trỏ mcp9808_dev từ đối tượng i_cdev, sau đó cất vào file->private_data để dùng sau
    file->private_data = container_of(inode->i_cdev, struct mcp9808_dev, cdev);
    return 0; // Trả về 0 báo hiệu mở file thành công
}

// Hàm này được kích hoạt khi user-space yêu cầu đọc dữ liệu từ file
static ssize_t mcp9808_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
    struct mcp9808_dev *mcp_dev = file->private_data; // Lấy lại dữ liệu cảm biến đang được thao tác
    int ret, temp_raw, temp_mc;                       // Các biến lưu mã lỗi và giá trị nhiệt độ
    char out_str[64];                                 // Bộ đệm chứa chuỗi text sẽ in ra màn hình
    int len;                                          // Chiều dài chuỗi sinh ra

    // Nếu con trỏ đọc (offset) đã vượt qua 0, nghĩa là đã gửi dữ liệu xong, trả về 0 để kết thúc lệnh cat
    if (*offset > 0) return 0;

    // Đọc 2 byte từ thanh ghi nhiệt độ, i2c_smbus tự động hoán đổi byte (Big Endian sang Little Endian)
    ret = i2c_smbus_read_word_swapped(mcp_dev->client, MCP9808_REG_TEMP);
    if (ret < 0) return ret; // Nếu không đọc được (ví dụ lỏng dây I2C), trả về ngay mã lỗi

    temp_raw = ret & 0x1FFF; // Dùng mask 0x1FFF để bỏ 3 bit cờ trạng thái (bit 13, 14, 15), chỉ lấy 13 bit dữ liệu
    
    if (temp_raw & 0x1000) { // Kiểm tra bit số 12 (Bit dấu). Nếu bằng 1 nghĩa là nhiệt độ đang bị âm
        // Tính nhiệt độ âm theo công thức từ Datasheet, quy đổi ra đơn vị milli-độ C
        temp_mc = -((256000 - ((temp_raw & 0x0FFF) * 625) / 10));
    } else {
        // Tính nhiệt độ dương theo công thức, quy đổi ra đơn vị milli-độ C
        temp_mc = (temp_raw * 625) / 10;
    }

    // Ghép dữ liệu thành chuỗi có kèm theo địa chỉ Slave để phân biệt (VD: "Sensor [0x18]: 25.500 C")
    len = snprintf(out_str, sizeof(out_str), "Sensor [0x%02x]: %d.%03d C\n", 
                   mcp_dev->client->addr, temp_mc / 1000, abs(temp_mc % 1000));

    // Copy chuỗi văn bản từ RAM của Kernel đưa ra vùng nhớ của User để hiển thị
    if (copy_to_user(user_buffer, out_str, len)) return -EFAULT; // Báo lỗi nếu quá trình copy thất bại

    *offset = len; // Cập nhật vị trí đã đọc được để báo cho Kernel biết đã hoàn thành
    return len;    // Trả về số byte văn bản vừa gửi thành công
}

// Hàm này được kích hoạt khi user-space dùng lệnh `echo` để ghi chuỗi vào file
static ssize_t mcp9808_write(struct file *file, const char __user *user_buffer, size_t size, loff_t *offset)
{
    struct mcp9808_dev *mcp_dev = file->private_data; // Lấy dữ liệu cảm biến đang thao tác
    char buf[8];                                      // Bộ đệm hứng chuỗi lệnh từ User gửi xuống
    int ret;                                          // Biến chứa mã lỗi/trạng thái
    u16 config_val;                                   // Biến lưu giữ nguyên trạng 16-bit thanh ghi cấu hình

    if (size > sizeof(buf) - 1) return -EINVAL;       // Nếu User gửi chuỗi quá dài (lớn hơn 7 ký tự) thì từ chối ngay
    if (copy_from_user(buf, user_buffer, size)) return -EFAULT; // Lấy chuỗi lệnh của User vào biến buf

    buf[size] = '\0'; // Đảm bảo chuỗi lệnh vừa nhận là một C-String chuẩn (kết thúc bằng Null)

    // Đọc giá trị Cấu hình (0x01) hiện tại của chip để không làm mất các cài đặt mặc định khác
    ret = i2c_smbus_read_word_swapped(mcp_dev->client, MCP9808_REG_CONFIG);
    if (ret < 0) return ret; // Báo lỗi nếu việc đọc thất bại
    config_val = ret;        // Gán dữ liệu đọc được vào biến chờ sửa đổi

    if (buf[0] == '1') {     // Nếu User gõ lệnh `echo 1`
        config_val |= 0x0100; // Dùng phép OR để Set bit số 8 (SHDN) lên 1, đưa IC vào Shutdown
        pr_info("TuyenHV1: Sensor [0x%02x] -> Entering Shutdown Mode\n", mcp_dev->client->addr); // Báo log tiếng Anh
    } else if (buf[0] == '0') { // Nếu User gõ lệnh `echo 0`
        config_val &= ~0x0100;  // Dùng phép AND và NOT để Xóa bit số 8 về 0, Đánh thức IC
        pr_info("TuyenHV1: Sensor [0x%02x] -> Waking up\n", mcp_dev->client->addr); // Báo log tiếng Anh
    } else {
        return -EINVAL; // Nếu gửi chuỗi không phải 0 hoặc 1 thì trả về lỗi thông số không hợp lệ
    }

    // Ghi đè giá trị cấu hình mới xuống phần cứng thông qua đường truyền I2C
    ret = i2c_smbus_write_word_swapped(mcp_dev->client, MCP9808_REG_CONFIG, config_val);
    if (ret < 0) return ret; // Nếu việc ghi thất bại, trả lại mã lỗi

    return size; // Trả về số byte lệnh đã xử lý xong
}

// Khai báo tập hợp các lệnh tương tác File (VFS) map vào các hàm chúng ta vừa code
static const struct file_operations mcp9808_fops = {
    .owner   = THIS_MODULE, // Đánh dấu Module này đang giữ file operation (tránh bị rmmod khi file đang mở)
    .open    = mcp9808_open, // Trỏ hàm mở file
    .read    = mcp9808_read, // Trỏ hàm đọc file
    .write   = mcp9808_write, // Trỏ hàm ghi file
};

// Hàm cực kỳ quan trọng, tự động chạy khi Kernel tìm thấy địa chỉ I2C khớp với Device Tree
static int mcp9808_probe(struct i2c_client *client)
{
    struct mcp9808_dev *mcp_dev; // Con trỏ để xin bộ nhớ quản lý cho cảm biến này
    int ret;                     // Biến lưu trạng thái
    dev_t dev_num;               // Số hiệu Device (Gộp giữa Major và Minor)

    // Đọc thử Manufacturer ID từ thanh ghi 0x06 để xác minh đúng là chip của Microchip hay không
    ret = i2c_smbus_read_word_swapped(client, MCP9808_REG_MANUF_ID);
    if (ret != 0x0054) return -ENODEV; // Nếu không phải 0x0054, coi như phần cứng cắm sai, hủy Probe

    // Xin Kernel cấp phát bộ nhớ động an toàn (Tự động giải phóng nếu module lỗi)
    mcp_dev = devm_kzalloc(&client->dev, sizeof(*mcp_dev), GFP_KERNEL);
    if (!mcp_dev) return -ENOMEM; // Báo lỗi hết RAM nếu không cấp phát được
    mcp_dev->client = client;     // Lưu giữ con trỏ phần cứng vào struct

    // VÙNG KHỞI TẠO DÙNG CHUNG: Chỉ chạy khi cắm con cảm biến ĐẦU TIÊN (Lúc này class chưa tồn tại)
    if (!mcp9808_class) {
        // Xin Kernel cấp sẵn dải 8 cặp số Major/Minor cho module này
        ret = alloc_chrdev_region(&mcp9808_base_devt, 0, 8, DRIVER_NAME);
        if (ret < 0) return ret; // Thoát nếu không xin được

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0) // Biên dịch với Kernel cũ (Ví dụ: 5.x, 6.1)
        mcp9808_class = class_create(THIS_MODULE, CLASS_NAME); // Lệnh tạo Class phải truyền THIS_MODULE
#else // Biên dịch với Kernel cực mới (Từ 6.4 trở lên)
        mcp9808_class = class_create(CLASS_NAME); // Hàm tạo Class đã bỏ tham số THIS_MODULE
#endif
        if (IS_ERR(mcp9808_class)) { // Nếu Kernel tạo class thất bại
            unregister_chrdev_region(mcp9808_base_devt, 8); // Phải trả lại dải số Major/Minor đã xin
            return PTR_ERR(mcp9808_class); // Thoát và ném ra lỗi
        }
    }

    // Gán 1 Minor Number cho con cảm biến này và tự động tăng số đếm lên 1 cho con tiếp theo
    mcp_dev->minor_id = minor_count++; 
    // Dùng macro MKDEV để ghép số Major chung và số Minor riêng thành ID phần cứng duy nhất
    dev_num = MKDEV(MAJOR(mcp9808_base_devt), mcp_dev->minor_id);

    cdev_init(&mcp_dev->cdev, &mcp9808_fops); // Khởi tạo cdev, nạp danh sách các hàm read/write vào
    cdev_add(&mcp_dev->cdev, dev_num, 1);     // Đăng ký quyền kiểm soát ID phần cứng này với Kernel

    // Lệnh này yêu cầu Linux tạo ngay file vào /dev/ với định dạng mcp9808_[địa_chỉ_i2c_hệ_hex]
    mcp_dev->mcp_device = device_create(mcp9808_class, &client->dev, dev_num, NULL, 
                                        "mcp9808_%02x", client->addr);

    // Cất ngược con trỏ dữ liệu mcp_dev vào lại i2c_client để hàm remove sau này có thể tìm lại
    i2c_set_clientdata(client, mcp_dev); 
    // In log tiếng Anh báo cáo Kernel đã mount thành công file
    pr_info("TuyenHV1: Created device node /dev/mcp9808_%02x\n", client->addr);

    return 0; // Trả về 0 báo hiệu Probe chạy thành công 100%
}

// Hàm này chạy khi rút cảm biến, tháo Device Tree Overlay, hoặc gỡ Driver bằng lệnh rmmod
static void mcp9808_remove(struct i2c_client *client)
{
    // Lôi lại con trỏ mcp_dev ra từ client
    struct mcp9808_dev *mcp_dev = i2c_get_clientdata(client);
    // Tính lại Device Number của con cảm biến đang bị tháo
    dev_t dev_num = MKDEV(MAJOR(mcp9808_base_devt), mcp_dev->minor_id);

    device_destroy(mcp9808_class, dev_num); // Xóa file /dev/mcp9808_xx của riêng con cảm biến này
    cdev_del(&mcp_dev->cdev);               // Hủy bỏ quyền kiểm soát Character Device của nó
    // In log tiếng Anh báo hiệu đã xóa file thiết bị
    pr_info("TuyenHV1: Removed device node /dev/mcp9808_%02x\n", client->addr);

    // VÙNG DỌN DẸP CHUNG: Giảm biến đếm. Nếu bằng 0 thì chứng tỏ đã tháo con cảm biến cuối cùng ra khỏi bus
    minor_count--;
    if (minor_count == 0) { 
        class_destroy(mcp9808_class); // Phá hủy hoàn toàn thư mục Class dùng chung
        unregister_chrdev_region(mcp9808_base_devt, 8); // Trả lại toàn bộ dải số phần cứng cho Kernel
        mcp9808_class = NULL; // Reset con trỏ Class về rỗng, để sau này cắm lại sẽ Init lại từ đầu
        // In log tiếng Anh báo hiệu dọn dẹp sạch sẽ tài nguyên
        pr_info("TuyenHV1: Cleaned up common Class resources\n"); 
    }
}

// Bảng Device ID hỗ trợ I2C System cơ bản (cách cũ)
static const struct i2c_device_id mcp9808_id[] = {
    { "mcp9808", 0 }, // Khai báo tên thiết bị để Kernel so khớp
    { }               // Phần tử rỗng báo hiệu kết thúc mảng
};
MODULE_DEVICE_TABLE(i2c, mcp9808_id); // Cấp phép xuất bảng ID này ra cho hệ thống Kernel

// Bảng Match Table hỗ trợ Device Tree (cách chuẩn hiện đại)
static const struct of_device_id mcp9808_of_match[] = {
    { .compatible = "microchip,mcp9808" }, // Chuỗi này bắt buộc phải trùng 100% với file .dts/.dtsi
    { }                                    // Phần tử rỗng báo hiệu kết thúc mảng
};
MODULE_DEVICE_TABLE(of, mcp9808_of_match); // Cấp phép xuất bảng Open Firmware này cho Kernel

// Khung xương chính (Struct) của một I2C Driver
static struct i2c_driver mcp9808_driver = {
    .driver = {
        .name = DRIVER_NAME,                 // Tên nội bộ của Driver
        .of_match_table = mcp9808_of_match,  // Nối vào bảng Device Tree
    },
    .probe = mcp9808_probe,                  // Nối hàm Probe (Chạy khi cắm)
    .remove = mcp9808_remove,                // Nối hàm Remove (Chạy khi rút)
    .id_table = mcp9808_id,                  // Nối vào bảng ID cũ
};

// Macro thần thánh: Tự động khởi tạo module_init() và dọn dẹp module_exit() mà không cần code tay
module_i2c_driver(mcp9808_driver);

MODULE_LICENSE("GPL"); // Khai báo License GPL v2 để không bị chặn bởi cơ chế chống nguồn đóng của Linux
MODULE_AUTHOR("Hoang Van Tuyen <tuyenmapab@gmail.com>"); // Tác giả Đồ án
MODULE_DESCRIPTION("Final project with Multiple MCP9808 - Fully Featured"); // Chú thích ngắn gọn chức năng module

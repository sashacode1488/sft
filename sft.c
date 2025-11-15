#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <dirent.h>
#include <mntent.h>
#endif

#define BUFFER_SIZE 1048576  
#define MAX_DEVICES 32

typedef struct {
    char path[256];
    char name[256];
    long long size;
    int is_removable;
} DeviceInfo;

void print_disclaimer() {
    printf("\n");
    printf("================================================================\n");
    printf("           SFT - Small Format Tool v1.0                        \n");
    printf("================================================================\n");
    printf("\n");
    printf("WARNING! IMPORTANT DISCLAIMER!\n");
    printf("----------------------------------------------------------------\n");
    printf("The author of this program is NOT RESPONSIBLE for:\n");
    printf("  * Lost data\n");
    printf("  * Damaged devices\n");
    printf("  * Any other consequences of using this program\n");
    printf("\n");
    printf("All operations are IRREVERSIBLE!\n");
    printf("Make sure you have selected the CORRECT device!\n");
    printf("----------------------------------------------------------------\n");
    printf("\n");
}

void print_menu() {
    printf("\n========================================\n");
    printf("            Main Menu                  \n");
    printf("========================================\n");
    printf(" 1. MBR Destruction (destroy MBR)     \n");
    printf(" 2. Full Format (complete format)     \n");
    printf(" 3. Fast Format (quick format)        \n");
    printf(" 4. Disk Image (create disk image)    \n");
    printf(" 5. Refresh device list                \n");
    printf(" 6. Exit                               \n");
    printf("========================================\n");
    printf("\nYour choice: ");
}

#ifdef _WIN32

int get_physical_drive_number(const char* drive_path, int* drive_number) {
    char volume_path[MAX_PATH];
    sprintf(volume_path, "\\\\.\\%c:", drive_path[0]);

    HANDLE hDevice = CreateFileA(volume_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        return 0;
    }

    STORAGE_DEVICE_NUMBER device_number;
    DWORD bytes_returned;

    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
        &device_number, sizeof(device_number), &bytes_returned, NULL)) {
        CloseHandle(hDevice);
        return 0;
    }

    *drive_number = device_number.DeviceNumber;
    CloseHandle(hDevice);
    return 1;
}

int is_removable_drive_win(const char* drive_path) {
    int drive_num;
    if (!get_physical_drive_number(drive_path, &drive_num)) {
        return 0;
    }

    char physical_drive[MAX_PATH];
    sprintf(physical_drive, "\\\\.\\PhysicalDrive%d", drive_num);

    HANDLE hDevice = CreateFileA(physical_drive, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        return 0;
    }

    STORAGE_PROPERTY_QUERY query;
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    BYTE out_buffer[1024];
    DWORD bytes_returned;
    PSTORAGE_DEVICE_DESCRIPTOR desc = (PSTORAGE_DEVICE_DESCRIPTOR)out_buffer;

    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        out_buffer, sizeof(out_buffer), &bytes_returned, NULL)) {
        int is_removable = desc->RemovableMedia || desc->BusType == BusTypeUsb;
        CloseHandle(hDevice);
        return is_removable;
    }

    CloseHandle(hDevice);
    return 0;
}

int list_removable_devices_win(DeviceInfo devices[], int max_devices) {
    int count = 0;
    DWORD drives = GetLogicalDrives();

    for (int i = 0; i < 26 && count < max_devices; i++) {
        if (drives & (1 << i)) {
            char drive_letter[4];
            sprintf(drive_letter, "%c:", 'A' + i);

            UINT drive_type = GetDriveTypeA(drive_letter);

            if (drive_type == DRIVE_REMOVABLE || is_removable_drive_win(drive_letter)) {

                int drive_num;
                if (get_physical_drive_number(drive_letter, &drive_num)) {
                    sprintf(devices[count].path, "\\\\.\\PhysicalDrive%d", drive_num);
                    sprintf(devices[count].name, "%c: -> PhysicalDrive%d (Removable)", 'A' + i, drive_num);
                }
                else {
                    sprintf(devices[count].path, "\\\\.\\%c:", 'A' + i);
                    sprintf(devices[count].name, "%c: (Removable)", 'A' + i);
                }

                char root_path[4];
                sprintf(root_path, "%c:\\", 'A' + i);
                ULARGE_INTEGER total_bytes;

                if (GetDiskFreeSpaceExA(root_path, NULL, &total_bytes, NULL)) {
                    devices[count].size = total_bytes.QuadPart;
                }
                else {

                    HANDLE hDevice = CreateFileA(devices[count].path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
                    if (hDevice != INVALID_HANDLE_VALUE) {
                        DISK_GEOMETRY_EX geometry;
                        DWORD bytes_returned;
                        if (DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                            &geometry, sizeof(geometry), &bytes_returned, NULL)) {
                            devices[count].size = geometry.DiskSize.QuadPart;
                        }
                        else {
                            devices[count].size = 0;
                        }
                        CloseHandle(hDevice);
                    }
                    else {
                        devices[count].size = 0;
                    }
                }

                devices[count].is_removable = 1;
                count++;
            }
        }
    }

    return count;
}

#else

int is_removable_drive_linux(const char* device) {
    char sys_path[512];
    char removable_path[512];
    char device_name[256];

    const char* last_slash = strrchr(device, '/');
    if (last_slash) {
        strcpy(device_name, last_slash + 1);
    }
    else {
        strcpy(device_name, device);
    }

    int len = strlen(device_name);
    while (len > 0 && device_name[len - 1] >= '0' && device_name[len - 1] <= '9') {
        device_name[--len] = '\0';
    }

    sprintf(removable_path, "/sys/block/%s/removable", device_name);

    FILE* f = fopen(removable_path, "r");
    if (!f) {
        return 0;
    }

    int removable = 0;
    fscanf(f, "%d", &removable);
    fclose(f);

    return removable;
}

int list_removable_devices_linux(DeviceInfo devices[], int max_devices) {
    int count = 0;
    DIR* dir = opendir("/sys/block");

    if (!dir) {
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && count < max_devices) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char device_path[512];
        sprintf(device_path, "/dev/%s", entry->d_name);

        if (is_removable_drive_linux(device_path)) {
            strcpy(devices[count].path, device_path);

            char removable_path[512];
            sprintf(removable_path, "/sys/block/%s/size", entry->d_name);

            FILE* f = fopen(removable_path, "r");
            if (f) {
                long long sectors;
                fscanf(f, "%lld", &sectors);
                devices[count].size = sectors * 512;
                fclose(f);
            }
            else {
                devices[count].size = 0;
            }

            sprintf(devices[count].name, "%s (Removable)", entry->d_name);
            devices[count].is_removable = 1;
            count++;
        }
    }

    closedir(dir);
    return count;
}

#endif

int list_removable_devices(DeviceInfo devices[], int max_devices) {
#ifdef _WIN32
    return list_removable_devices_win(devices, max_devices);
#else
    return list_removable_devices_linux(devices, max_devices);
#endif
}

void display_devices(DeviceInfo devices[], int count) {
    printf("\n========================================\n");
    printf("   Detected Removable Devices          \n");
    printf("========================================\n");

    if (count == 0) {
        printf("No removable devices detected.\n");
        printf("----------------------------------------\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        double size_gb = devices[i].size / (1024.0 * 1024.0 * 1024.0);
        printf(" [%d] %s\n", i + 1, devices[i].name);
        printf("     Path: %s\n", devices[i].path);
        printf("     Size: %.2f GB\n", size_gb);
        printf("----------------------------------------\n");
    }
}

long long get_disk_size(const char* device) {
#ifdef _WIN32
    HANDLE hDevice = CreateFileA(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        return -1;
    }

    DISK_GEOMETRY_EX geometry;
    DWORD bytes_returned;
    if (!DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
        &geometry, sizeof(geometry), &bytes_returned, NULL)) {
        CloseHandle(hDevice);
        return -1;
    }

    CloseHandle(hDevice);
    return geometry.DiskSize.QuadPart;
#else
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    long long size;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return size;
#endif
}

int mbr_destruction(const char* device) {
    printf("\nMBR DESTRUCTION\n");
    printf("This mode will destroy the first 512 bytes (MBR) on disk %s\n", device);
    printf("The disk will become unusable until reformatted!\n");
    printf("\nAre you ABSOLUTELY SURE? (type 'YES' in capital letters): ");

    char confirm[10];
    scanf("%s", confirm);

    if (strcmp(confirm, "YES") != 0) {
        printf("Operation cancelled.\n");
        return 0;
    }

#ifdef _WIN32
    HANDLE hDevice = CreateFileA(device, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("Error opening device!\n");
        return -1;
    }

    unsigned char zeros[512] = { 0 };
    DWORD bytes_written;
    if (!WriteFile(hDevice, zeros, 512, &bytes_written, NULL)) {
        printf("Write error!\n");
        CloseHandle(hDevice);
        return -1;
    }

    CloseHandle(hDevice);
#else
    int fd = open(device, O_WRONLY | O_SYNC);
    if (fd < 0) {
        printf("Error opening device! Run the program with root privileges.\n");
        return -1;
    }

    unsigned char zeros[512] = { 0 };
    if (write(fd, zeros, 512) != 512) {
        printf("Write error!\n");
        close(fd);
        return -1;
    }

    fsync(fd);
    close(fd);
#endif

    printf("MBR successfully destroyed!\n");
    return 0;
}

int full_format(const char* device) {
    printf("\nFULL FORMAT\n");

    long long disk_size = get_disk_size(device);
    if (disk_size < 0) {
        printf("Unable to determine disk size!\n");
        return -1;
    }

    printf("Disk size: %.2f GB\n", disk_size / (1024.0 * 1024.0 * 1024.0));
    printf("This mode will overwrite ALL data with zeros!\n");
    printf("This may take a LONG time!\n");
    printf("\nAre you ABSOLUTELY SURE? (type 'YES' in capital letters): ");

    char confirm[10];
    scanf("%s", confirm);

    if (strcmp(confirm, "YES") != 0) {
        printf("Operation cancelled.\n");
        return 0;
    }

#ifdef _WIN32
    HANDLE hDevice = CreateFileA(device, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("Error opening device! Error code: %lu\n", GetLastError());
        printf("Make sure you run the program as Administrator.\n");
        return -1;
    }

    DWORD bytes_returned;
    DeviceIoControl(hDevice, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytes_returned, NULL);
    DeviceIoControl(hDevice, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytes_returned, NULL);
#else
    int fd = open(device, O_WRONLY | O_SYNC);
    if (fd < 0) {
        printf("Error opening device! Run the program with root privileges.\n");
        return -1;
    }
#endif

    unsigned char* buffer = (unsigned char*)calloc(BUFFER_SIZE, 1);
    if (!buffer) {
        printf("Memory allocation error!\n");
#ifdef _WIN32
        CloseHandle(hDevice);
#else
        close(fd);
#endif
        return -1;
    }

    long long written = 0;
    printf("\nProgress: ");
    fflush(stdout);

    while (written < disk_size) {
        long long to_write = (disk_size - written > BUFFER_SIZE) ? BUFFER_SIZE : (disk_size - written);

        to_write = (to_write / 512) * 512;
        if (to_write == 0 && written < disk_size) {
            to_write = 512;
        }

#ifdef _WIN32
        DWORD bytes_written;
        if (!WriteFile(hDevice, buffer, (DWORD)to_write, &bytes_written, NULL)) {
            DWORD error = GetLastError();
            printf("\nWrite error! Error code: %lu\n", error);
            if (error == ERROR_WRITE_PROTECT) {
                printf("The device is write-protected!\n");
            }
            break;
        }
        written += bytes_written;
#else
        ssize_t bytes_written = write(fd, buffer, to_write);
        if (bytes_written < 0) {
            printf("\nWrite error!\n");
            break;
        }
        written += bytes_written;
#endif

        int percent = (int)((written * 100) / disk_size);
        printf("\rProgress: %d%% [%lld / %lld MB]", percent,
            written / (1024 * 1024), disk_size / (1024 * 1024));
        fflush(stdout);
    }

    printf("\n");
    free(buffer);

#ifdef _WIN32

    DeviceIoControl(hDevice, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytes_returned, NULL);
    CloseHandle(hDevice);
#else
    fsync(fd);
    close(fd);
#endif

    printf("Full format completed!\n");
    return 0;
}

int fast_format(const char* device) {
    printf("\nFAST FORMAT\n");
    printf("This mode will destroy the partition table on disk %s\n", device);
    printf("\nAre you sure? (type 'YES' in capital letters): ");

    char confirm[10];
    scanf("%s", confirm);

    if (strcmp(confirm, "YES") != 0) {
        printf("Operation cancelled.\n");
        return 0;
    }

#ifdef _WIN32
    HANDLE hDevice = CreateFileA(device, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("Error opening device!\n");
        return -1;
    }

    unsigned char zeros[4096] = { 0 };
    DWORD bytes_written;
    if (!WriteFile(hDevice, zeros, 4096, &bytes_written, NULL)) {
        printf("Write error!\n");
        CloseHandle(hDevice);
        return -1;
    }

    CloseHandle(hDevice);
#else
    int fd = open(device, O_WRONLY | O_SYNC);
    if (fd < 0) {
        printf("Error opening device! Run the program with root privileges.\n");
        return -1;
    }

    unsigned char zeros[4096] = { 0 };
    if (write(fd, zeros, 4096) != 4096) {
        printf("Write error!\n");
        close(fd);
        return -1;
    }

    fsync(fd);
    close(fd);
#endif

    printf("Fast format completed!\n");
    return 0;
}

int disk_image(const char* device, const char* image_path) {
    printf("\nDISK IMAGE\n");

    long long disk_size = get_disk_size(device);
    if (disk_size < 0) {
        printf("Unable to determine disk size!\n");
        return -1;
    }

    printf("Disk size: %.2f GB\n", disk_size / (1024.0 * 1024.0 * 1024.0));
    printf("Creating an image may take a long time!\n");

#ifdef _WIN32
    HANDLE hDevice = CreateFileA(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("Error opening device!\n");
        return -1;
    }
#else
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        printf("Error opening device! Run the program with root privileges.\n");
        return -1;
    }
#endif

    FILE* img_file = fopen(image_path, "wb");
    if (!img_file) {
        printf("Failed to create image file!\n");
#ifdef _WIN32
        CloseHandle(hDevice);
#else
        close(fd);
#endif
        return -1;
    }

    unsigned char* buffer = (unsigned char*)malloc(BUFFER_SIZE);
    if (!buffer) {
        printf("Memory allocation error!\n");
        fclose(img_file);
#ifdef _WIN32
        CloseHandle(hDevice);
#else
        close(fd);
#endif
        return -1;
    }

    long long read_total = 0;
    printf("\nProgress: ");

    while (read_total < disk_size) {
        long long to_read = (disk_size - read_total > BUFFER_SIZE) ? BUFFER_SIZE : (disk_size - read_total);

#ifdef _WIN32
        DWORD bytes_read;
        if (!ReadFile(hDevice, buffer, to_read, &bytes_read, NULL) || bytes_read == 0) {
            break;
        }
#else
        ssize_t bytes_read = read(fd, buffer, to_read);
        if (bytes_read <= 0) {
            break;
        }
#endif

        fwrite(buffer, 1, bytes_read, img_file);
        read_total += bytes_read;

        int percent = (int)((read_total * 100) / disk_size);
        printf("\rProgress: %d%% [%lld / %lld MB]", percent,
            read_total / (1024 * 1024), disk_size / (1024 * 1024));
        fflush(stdout);
    }

    printf("\n");
    free(buffer);
    fclose(img_file);

#ifdef _WIN32
    CloseHandle(hDevice);
#else
    close(fd);
#endif

    printf("Disk image successfully created: %s\n", image_path);
    return 0;
}

int main() {
    DeviceInfo devices[MAX_DEVICES];
    int device_count = 0;

    print_disclaimer();

    printf("Press Enter to continue or Ctrl+C to exit...");
    getchar();

    device_count = list_removable_devices(devices, MAX_DEVICES);
    display_devices(devices, device_count);

    while (1) {
        print_menu();

        int choice;
        scanf("%d", &choice);
        getchar();

        if (choice == 6) {
            printf("\nGoodbye!\n");
            break;
        }

        if (choice == 5) {
            device_count = list_removable_devices(devices, MAX_DEVICES);
            display_devices(devices, device_count);
            continue;
        }

        if (device_count == 0) {
            printf("\nNo removable devices available. Please refresh the device list.\n");
            continue;
        }

        printf("\nSelect device number (1-%d): ", device_count);
        int device_choice;
        scanf("%d", &device_choice);
        getchar();

        if (device_choice < 1 || device_choice > device_count) {
            printf("Invalid device number!\n");
            continue;
        }

        const char* selected_device = devices[device_choice - 1].path;

        switch (choice) {
        case 1:
            mbr_destruction(selected_device);
            break;
        case 2:
            full_format(selected_device);
            break;
        case 3:
            fast_format(selected_device);
            break;
        case 4: {
            char image_path[512];
            printf("Enter path to save the image (e.g., disk_image.img): ");
            scanf("%s", image_path);
            disk_image(selected_device, image_path);
            break;
        }
        default:
            printf("Invalid choice!\n");
        }

        printf("\nPress Enter to continue...");
        getchar();
    }

    return 0;
}
#include "64drive.h"

static int verbosity = 0;

static struct option long_options[] = {
    {"bank",         required_argument, 0, 'b'},
    {"cic",          required_argument, 0, 'c'},
    {"dump",         required_argument, 0, 'd'},
    {"help",         no_argument,       0, 'h'},
    {"info",         no_argument,       0, 'i'},
    {"load",         required_argument, 0, 'l'},
    {"list-devices", no_argument,       0, 'L'},
    {"offset",       required_argument, 0, 'o'},
    {"quiet",        no_argument,       0, 'q'},
    {"size",         required_argument, 0, 's'},
    {"verbose",      no_argument,       0, 'v'},
    {0, 0, 0, 0}
};

static struct {
    const char *name;
    int bank;
} banks[] = {
    {"rom",     BANK_CARTROM},
    {"sram256", BANK_SRAM256},
    {"sram768", BANK_SRAM768},
    {"flash",   BANK_FLASHRAM1M},
    {"pokemon", BANK_FLASHPKM1M},
    {"eeprom",  BANK_EEPROM16},
    {NULL, 0}
};

static struct {
    int num;
    int cic;
    uint32_t crc32; //of bootcode
    const char *desc;
} cic_types[] = { //XXX missing CRCs
    {6101, CIC_6101, 0x6170A4A1, "Star Fox"},
    {6102, CIC_6102, 0x90BB6CB5, "most NTSC games"},
    {7101, CIC_7101, 0xFFFFFFFF, "most PAL games"},
    {7102, CIC_7102, 0xFFFFFFFF, "Lylat Wars"},
    { 103, CIC_X103, 0x0B050EE0, "covers 6103 and 7103"},
    { 105, CIC_X105, 0x98BC2C86, "covers 6105 and 7105"},
    { 106, CIC_X106, 0xACC8580A, "covers 6106 and 7106"},
    {5101, CIC_5101, 0xFFFFFFFF, "Aleck64"},
    //8303: JP 64DD (not dumped)
    {0, 0, 0, NULL}
};

uint32_t swap_endian(uint32_t val) {
    return ((val << 24)) |
           ((val << 8) & 0x00ff0000) |
           ((val >> 8) & 0x0000ff00) |
           ((val >> 24));
}


int fail_ftdi(struct ftdi_context* ftdi, const char *msg) {
    fprintf(stderr, "%s: %s\n", msg, ftdi_get_error_string(ftdi));
    return EXIT_FAILURE;
}


uint32_t crc32(const uint8_t *data, size_t len) {
    //copied from http://n64dev.org/n64crc.html

    static uint32_t crc_table[256];
    static int isInit = 0;
    if(!isInit) { //generate CRC table
        uint32_t poly = 0xEDB88320;
        for(int i = 0; i < 256; i++) {
            uint32_t crc = i;
            for(int j = 8; j > 0; j--) {
                if (crc & 1) crc = (crc >> 1) ^ poly;
                else crc >>= 1;
            }
            crc_table[i] = crc;
        }
        isInit = 1;
    }

    uint32_t crc = ~0;
	for(size_t i = 0; i < len; i++) {
		crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xFF];
	}
	return ~crc;
}


int get_cic(FILE *rom) {
    //copied from http://n64dev.org/n64crc.html
    uint8_t data[0xFC0];
    fseek(rom, 0x40, SEEK_SET);
    fread(data, 1, sizeof(data), rom); //read bootcode
    uint32_t crc = crc32(data, sizeof(data));
    if(verbosity > 0) printf(" * Bootcode CRC32: 0x%08X\n", crc);
    for(int i=0; cic_types[i].num; i++) {
        if(cic_types[i].crc32 == crc) return i;
    }
    return -1;
}


int list_devices(struct ftdi_context* ftdi) {
    struct ftdi_device_list *devices;
    int nDevices = ftdi_usb_find_all(ftdi, &devices, 0, 0);
    if(nDevices < 0) {
        fail_ftdi(ftdi, "ftdi_usb_find_all");
        return nDevices;
    }
    printf(" * Found %d devices\n", nDevices);

    for(int i=0; i<nDevices; i++) {
        char manufacturer[8192], description[8192], serial[8192];
        int err = ftdi_usb_get_strings(ftdi, devices[i].dev,
            manufacturer, sizeof(manufacturer),
            description, sizeof(description),
            serial, sizeof(serial));

        if(err) {
            fprintf(stderr, "ftdi_usb_get_strings(device %d) failed: %s\n",
                i, ftdi_get_error_string(ftdi));
        }
        else {
            printf(" * Device %d: \"%s\", manuf \"%s\", serial \"%s\"\n", i,
                description, manufacturer, serial);
        }
    }

    ftdi_list_free(&devices);
    return nDevices;
}


int device_send_cmd(sixtyfourDrive *device, uint8_t cmd, uint8_t nParams,
uint32_t *params, uint8_t *resp, uint32_t respLen) {
    uint8_t tx_buf[32];

    memset(tx_buf, 0, sizeof(tx_buf));
    tx_buf[0] = cmd;
    tx_buf[1] = 'C';
    tx_buf[2] = 'M';
    tx_buf[3] = 'D';

    if(nParams >= sizeof(tx_buf) / sizeof(uint32_t)) {
        fprintf(stderr, "Too many params for command\n");
        return -1;
    }

    uint32_t *paramBuf = (uint32_t*)&tx_buf[4];
    for(int i=0; i<nParams; i++) {
        paramBuf[i] = swap_endian(params[i]);
    }

    if(verbosity > 2) printf(" * Sending command 0x%02X\n", cmd);

    int err = ftdi_write_data(device->ftdi, tx_buf, 4 + (nParams * 4));
    if(err <= 0) {
        fprintf(stderr, "device_send_cmd(0x%02X) write failed: %s\n",
            cmd, ftdi_get_error_string(device->ftdi));
        return err;
    }

    if(respLen > 0) {
        err = ftdi_read_data(device->ftdi, resp, respLen);
        if(err <= 0) {
            fprintf(stderr, "device_send_cmd(0x%02X) read failed: %s\n",
                cmd, ftdi_get_error_string(device->ftdi));
        }
    }

    return err;
}


int device_get_version(sixtyfourDrive *device) {
    uint8_t response[64];
    int err = device_send_cmd(device, DEV_CMD_GETVER, 0, NULL,
        response, sizeof(response));
    if(err <= 0) {
        fprintf(stderr, "device_get_version() failed: %s\n"
            "Try unplugging 64drive USB cable and turning off console.\n",
            ftdi_get_error_string(device->ftdi));
        return err;
    }

    int tries = 0;
    while(1) {
        int err = device_send_cmd(device, DEV_CMD_GETVER, 0, NULL,
            response, sizeof(response));
        if(err <= 0) {
            fprintf(stderr, "device_get_version() failed: %s\n",
                ftdi_get_error_string(device->ftdi));
            return err;
        }

        uint32_t *data = (uint32_t*)response;
        uint32_t magic = swap_endian(data[1]);
        if(magic == DEV_MAGIC) break;

        if(verbosity > 0) {
            fprintf(stderr, " ! incorrect magic 0x%08X, expected 0x%08X\n",
                magic, DEV_MAGIC);
        }

        if(++tries >= 4) {
            fprintf(stderr,
                "\nCommunication failure.\n"
                "Unplug USB cable, turn off N64, then try again.\n");
            return -1;
        }
    }

    /* if(verbosity > 0) {
        printf(" * HW revision: %c%c\n", response[0], response[1]);
    } */
    device->variant[0] = response[0];
    device->variant[1] = response[1];
    device->variant[2] = response[2];
    return (response[0] << 24) | (response[1] << 16) | (response[2] << 8);
}


int device_upload(sixtyfourDrive *device, FILE *file, int64_t size,
uint32_t offset, int bank) {
    /** Upload file to device.
     *  file:   File to upload.
     *  size:   Size to upload. If -1, upload entire file (minus seek position).
     *  offset: Offset to upload to.
     *  bank:   Bank to upload to.
     *  Will upload from the file's current seek position to the specified size.
     */

    if(size < 0) {
        int64_t cur = ftell(file);
        fseek(file, 0, SEEK_END);
        size = ftell(file) - cur;
        fseek(file, cur, SEEK_SET); //restore position
    }

    //determine ideal chunk size
    uint32_t chunkSize;
    if(size > 16 * 1024 * 1024) chunkSize = 32;
    else if(size > 2 * 1024 * 1024) chunkSize = 16;
    else chunkSize = 4;
    if(verbosity > 1) printf(" * Chunk size: %d => %d\n",
        chunkSize, (chunkSize * 128 * 1024) & 0xffffff);
    chunkSize *= 128 * 1024; // convert to megabytes
    if(chunkSize > size) chunkSize = size;

    uint8_t *buffer = (uint8_t*)malloc(chunkSize);
    if(!buffer) {
        fprintf(stderr, "device_upload(): out of memory\n");
        return -1;
    }

    int err = ftdi_write_data_set_chunksize(device->ftdi, chunkSize);
    if(err) {
        fprintf(stderr, "device_upload() set chunk size failed: %s\n",
            ftdi_get_error_string(device->ftdi));
        free(buffer);
        return err;
    }

    if(verbosity > 0) {
        printf(" * Uploading %" PRId64 " Kbytes to offset 0x%06X\n",
            size / 1024, offset);
    }
    for(int64_t readPos=0; readPos<size;) {
        fread(buffer, chunkSize, 1, file);

        uint32_t params[2] = {offset, (chunkSize & 0xffffff) | bank << 24};
        device_send_cmd(device, DEV_CMD_LOADRAM, 2, params, NULL, 0);

        int nSent = -1;
        for(int tries=0; tries<5; tries++) {
            nSent = ftdi_write_data(device->ftdi, buffer, chunkSize);
            if(nSent > 0) break;

            //wait, flush, retry
            usleep(10000);
            ftdi_usb_purge_buffers(device->ftdi);
        }
        if(nSent <= 0) {
            fprintf(stderr, "\ndevice_upload() write failed "
                "(after %" PRId64 " bytes): %s\n", readPos,
                ftdi_get_error_string(device->ftdi));
            free(buffer);
            return nSent;
        }

        offset += nSent;
        readPos += nSent;
        if(verbosity >= 0) {
            printf("\r * Uploading... %3" PRId64 "%%", (readPos * 100) / size);
            fflush(stdout);
        }
    }
    if(verbosity >= 0) printf("\r * Uploading... Done.\n");
    free(buffer);
    return 0;
}


int device_download(sixtyfourDrive *device, FILE *file, int64_t size,
uint32_t offset, int bank, bool standalone) {
    /** Download file from device.
     *  file:       File to write to.
     *  size:       Size to download.
     *  offset:     Offset to download from.
     *  bank:       Bank to download from.
     *  standalone: Standalone mode, i.e. read from attached cartridge
     */

    if(size < 0) {
        //XXX get bank size
        size = 256 * 1024 * 1024; //256 MBytes
    }

    //determine ideal chunk size
    uint32_t chunkSize;
    if(standalone) chunkSize = 512;
    else if(size > 16 * 1024 * 1024) chunkSize = 32;
    else if(size > 2 * 1024 * 1024) chunkSize = 16;
    else chunkSize = 4;
    if(verbosity > 1) printf(" * Chunk size: %d => %d\n",
        chunkSize, chunkSize * 128 * 1024);
    if(!standalone) chunkSize *= 128 * 1024; // convert to megabytes for RAM dump
    if(chunkSize > size) chunkSize = size;

    uint8_t *buffer = (uint8_t*)malloc(chunkSize);
    if(!buffer) {
        fprintf(stderr, "device_download(): out of memory\n");
        return -1;
    }

    int err = ftdi_read_data_set_chunksize(device->ftdi, chunkSize);
    if(err) {
        fprintf(stderr, "device_download() set chunk size failed: %s\n",
            ftdi_get_error_string(device->ftdi));
        free(buffer);
        return err;
    }

    if(verbosity > 0) printf(" * Downloading %" PRId64 " Kbytes\n", size / 1024);
    if (standalone) {
        if(verbosity > 0) printf(" * Entering standalone mode\n");
        uint8_t response[4];
        device_send_cmd(device, DEV_CMD_STD_ENTER, 0, NULL, response, sizeof(response));
    }

    for(int64_t readPos=0; readPos<size;) {
        if(standalone) {
            uint32_t params[2] = {offset | 0x10 << 24, chunkSize/4};
            device_send_cmd(device, DEV_CMD_PI_RD_BURST, 2, params, NULL, 0);
        } else {
            uint32_t params[2] = {offset, (chunkSize & 0xffffff) | bank << 24};
            device_send_cmd(device, DEV_CMD_DUMPRAM, 2, params, NULL, 0);
        }

        int nRecv = -1;
        for(int tries=0; tries<5; tries++) {
            nRecv = ftdi_read_data(device->ftdi, buffer, chunkSize);
            if(nRecv > 0) break;

            //wait, flush, retry
            usleep(10000);
            ftdi_usb_purge_buffers(device->ftdi);
        }
        if(nRecv <= 0) {
            fprintf(stderr, "\ndevice_download() read failed "
                "(after %" PRId64 " bytes): %s\n", readPos,
                ftdi_get_error_string(device->ftdi));
            free(buffer);
            return nRecv;
        }
        fwrite(buffer, nRecv, 1, file);

        offset += nRecv;
        readPos += nRecv;
        if(verbosity >= 0) {
            printf("\r * Downloading... %3" PRId64 "%%", (readPos * 100) / size);
            fflush(stdout);
        }
    }
    if(verbosity >= 0) printf("\r * Downloading... Done.\n");
    if(standalone) {
        if (verbosity > 0) printf(" * Leaving standalone mode");
        uint8_t response[4];
        device_send_cmd(device, DEV_CMD_STD_LEAVE, 0, NULL, response, sizeof(response));
    }
    free(buffer);
    return 0;
}


int device_set_cic(sixtyfourDrive *device, int cic) {
    if(device->variant[0] == 'A') {
        fprintf(stderr, "This device does not support changing CIC mode.\n");
        return -1;
    }

    if(verbosity > 0) {
        printf(" * Selecting CIC %d (#%d)\n", cic_types[cic].num, cic);
    }

    uint32_t param = (1 << 31) | cic;
    return device_send_cmd(device, DEV_CMD_SETCIC, 1, &param, NULL, 0);
}


int device_open(sixtyfourDrive *device) {
    //return device version: 2=HW2 1=HW1 0=not found
    static struct {
        uint16_t vid, pid;
        int version;
        const char *descr;
    } devices[] = {
        {0x0403, 0x6014, 2, "64drive USB device"},
        {0x0403, 0x6010, 1, "64drive USB device A"},
        {0x0403, 0x6010, 1, "64drive USB device"},
        {0, 0, 0, NULL}
    };

    for(int i=0; devices[i].vid; i++) {
        int err = ftdi_usb_open_desc(device->ftdi,
            devices[i].vid, devices[i].pid, devices[i].descr, NULL);
        if(!err) {
            device->version = devices[i].version;
            return device->version;
        }
        if(err != -3) {
            fprintf(stderr, "device_open(): %s\n",
                ftdi_get_error_string(device->ftdi));
        }
    }

    return 0;
}


int device_init(sixtyfourDrive *device) {
    if(verbosity > 1) printf(" * Resetting device\n");
    int err = ftdi_usb_reset(device->ftdi);
    if(err) return fail_ftdi(device->ftdi, "ftdi_usb_reset");

    if(device->version == 2) {
        if(verbosity > 1) printf(" * Setting synchronous mode\n");

        err = ftdi_set_bitmode(device->ftdi, 0xFF, BITMODE_RESET);
        if(err) return fail_ftdi(device->ftdi, "ftdi_set_bitmode(BITMODE_RESET)");

        err = ftdi_set_bitmode(device->ftdi, 0xFF, BITMODE_SYNCFF);
        if(err) return fail_ftdi(device->ftdi, "ftdi_set_bitmode(BITMODE_SYNCFF)");
    }
    err = ftdi_set_latency_timer(device->ftdi, 255);
    if(err) return fail_ftdi(device->ftdi, "ftdi_set_latency_timer");

    if(verbosity > 1) printf(" * Purging buffers\n");
    err = ftdi_usb_purge_buffers(device->ftdi);
    if(err) return fail_ftdi(device->ftdi, "ftdi_usb_purge_buffers");

    return 1;
}


int setup_device(sixtyfourDrive *device) {
    device->ftdi = ftdi_new();
    if(!device->ftdi) {
        return fail_ftdi(device->ftdi, "ftdi_new");
    }

    int ver = device_open(device);
    if(ver < 1) {
        fprintf(stderr, "64drive device not found.\n");
        ftdi_free(device->ftdi);
        return EXIT_FAILURE;
    }
    if(verbosity > 0) printf(" * Found 64drive version %d\n", device->version);

    if(!device_init(device)) {
        ftdi_deinit(device->ftdi);
        return EXIT_FAILURE;
    }

    if(device_get_version(device) <= 0) {
        ftdi_deinit(device->ftdi);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


void shutdown_device(sixtyfourDrive *device) {
    if(device->ftdi == NULL) return;
    ftdi_usb_close(device->ftdi);
    ftdi_deinit(device->ftdi);
}


void show_help() {
    printf(
        "64drive USB tool for Linux\n"
        "by Rena, 2017 May 02\n"
        "https://github.com/RenaKunisaki/64drive-usb-linux\n"
        "based on original USB tool by marshallh:\n"
        "http://64drive.retroactive.be/support.php\n"
        "\n"
        "usage: 64drive options...\n"
        "options:\n"
        "  -b, --bank BANK      up/download to specified bank (default: rom)\n"
        "  -c, --cic  CIC       set CIC type (HW2 RevB only)\n"
        "  -d, --dump FILE      download file from cartridge\n"
        "  -h, --help           show help and exit\n"
        "  -i, --info           show device info (version)\n"
        "  -l, --load FILE      upload file to cartridge\n"
        "  -L, --list-devices   list FTDI devices\n"
        "  -o, --offset OFFSET  upload to/download from specified offset "
        "(default: 0)\n"
        "  -q, --quiet          be quiet (no progress indicators)\n"
        "  -v, --verbose        be verbose (repeat for more verbosity)\n"
        "  -z, --size SIZE      up/download specified size "
        "(default: entire file)\n"
        "      (must be multiple of 512)\n"
        "\n"
        "CIC is one of:\n"
        "  auto (use before -l)\n"
    );
    for(int i=0; i<CIC_LAST; i++) {
        printf("  %4d (%s)\n", cic_types[i].num, cic_types[i].desc);
    }
    printf(
        "  CIC must be set correctly for the game to work.\n"
        "\n"
        "BANK is one of: rom, sram256, sram768, flash, pokemon, eeprom\n"
        " -\"pokemon\" is special-case flash for Pokemon Stadium 2\n"
        " -\"sram768\" is only used by Dezaemon 3D\n"
        "\n"
        "FILE is a file path, or \"-\" for stdin (for upload)/"
        "stdout (for download).\n"
        "\n"
        "-b sets the bank for ALL following up/downloads (until another -b).\n"
        "-o and -s set the offset and size for ONLY THE NEXT up/download.\n"
        "\n"
        "Args are processed in the order given, so eg:\n"
        "  64drive -l file.rom -b eeprom -l file.sav\n"
        "will upload file.rom to ROM and file.sav to EEPROM.\n"
    );
}



void setup_or_die(sixtyfourDrive *device) {
    static int is_setup = 0;
    if(is_setup) return;
    int err = setup_device(device);
    if(err) exit(err);
    is_setup = 1;
}


int main(int argc, char **argv) {
    sixtyfourDrive device;
    device.ftdi = NULL;

    int bank = BANK_CARTROM;
    int64_t fileSize = -1, fileOffset = 0;
    int autoCIC = 0;

    if(argc < 2) {
        show_help();
        return EXIT_SUCCESS;
    }

    while(1) {
        int c = getopt_long(argc, argv,
            "b:c:d:D:z:hil:Lo:qs:v", long_options, NULL);
        if(c < 0) break;
        switch(c) {
            case 'b': { //specify bank
                bank = -1;
                for(int i=0; banks[i].name; i++) {
                    if(!strcmp(optarg, banks[i].name)) {
                        bank = banks[i].bank;
                        break;
                    }
                }
                if(bank < 0) {
                    bank = atoi(optarg);
                    if(bank < 0 || bank >= BANK_LAST) { //Windows version compat
                        fprintf(stderr, "Invalid bank\n");
                        return EXIT_FAILURE;
                    }
                }
                break;
            }

            //B: update bootloader (not implemented)
            //in Windows version it's small b, but I already used that for bank
            //which is a much more commonly used option.

            case 'c': { //set CIC
                int cic = -1;
                if(!strcmp(optarg, "auto")) {
                    autoCIC = 1;
                }
                else {
                    int num = atoi(optarg);
                    for(int i=0; cic_types[i].num; i++) {
                        if((cic_types[i].num == num)
                        || (num < CIC_LAST && num == i)) {
                            //check for num == i for compatibility
                            //with Windows version; eg 3 = 7102
                            cic = cic_types[i].cic;
                            setup_or_die(&device);
                            device_set_cic(&device, cic_types[i].cic);
                            break;
                        }
                    }
                    if(cic < 0) {
                        fprintf(stderr, "Invalid CIC\n");
                        return EXIT_FAILURE;
                    }
                }
                break;
            }
            case 'D': //dump real cartridge
            case 'd': { //dump RAM
                setup_or_die(&device);

                FILE *file;
                if(!strcmp(optarg, "-")) { file = stdout; verbosity = -1; }
                else file = fopen(optarg, "wb");
                if(!file) {
                    fprintf(stderr, "Failed opening \"%s\": %s\n", optarg,
                        strerror(errno));
                    break;
                }
                device_download(&device, file, fileSize, fileOffset, bank, c == 'D');
                fclose(file);
                fileSize = -1;
                fileOffset = 0;
                break;
            }

            //f: update firmware (not implemented)

            case 'h': //help
                show_help();
                return EXIT_SUCCESS;

            case 'i': //info
                setup_or_die(&device);
                device_get_version(&device);
                printf("Device version: HW%d rev %c%c%c\n",
                    device.version, device.variant[0],
                    device.variant[1], device.variant[2]);
                break;

            case 'l': { //upload
                setup_or_die(&device);

                FILE *file;
                if(!strcmp(optarg, "-")) { file = stdin; verbosity = -1; }
                else file = fopen(optarg, "rb");
                if(!file) {
                    fprintf(stderr, "Failed opening \"%s\": %s\n", optarg,
                        strerror(errno));
                    break;
                }

                if(autoCIC) {
                    if(verbosity > 1) printf(" * Identifying CIC...\n");
                    int cic = get_cic(file);
                    if(cic < 0) {
                        fprintf(stderr, " ! Auto CIC selection failed - "
                            "unrecognized bootcode.\n");
                    }
                    else {
                        if(verbosity > 0) {
                            printf(" * Auto detected CIC: %d\n",
                                cic_types[cic].num);
                        }
                        device_set_cic(&device, cic_types[cic].cic);
                    }
                }

                device_upload(&device, file, fileSize, fileOffset, bank);
                fclose(file);
                fileSize = -1;
                fileOffset = 0;
                break;
            }

            case 'L': { //list devices
                if(device.ftdi) {
                    list_devices(device.ftdi);
                }
                else {
                    device.ftdi = ftdi_new();
                    list_devices(device.ftdi);
                    ftdi_deinit(device.ftdi);
                    device.ftdi = NULL;
                }
                break;
            }

            case 'o': //set offset
                fileOffset = strtoul(optarg, NULL, 0); //XXX detect error
                break;

            case 'q': //quiet
                verbosity = -1;
                break;

            //s: set save emulation type (not implemented)
            //was set size in old versions (changed to z)

            case 'v': //verbose
                verbosity++;
                break;

            case 'z': //set size
                fileSize = strtoul(optarg, NULL, 0); //XXX detect error
                //printf("fileSize = %d\n", fileSize);
                break;

            default:
                fprintf(stderr, "getopt returned 0x%02X '%c'\n", c, c);
        }
    }

    shutdown_device(&device);
    return EXIT_SUCCESS;
}

#define TYPE_BCM2835_FB "bcm2835_fb"
#define BCM2835_FB(obj) OBJECT_CHECK(Bcm2835FbState, (obj), TYPE_BCM2835_FB)

typedef struct {
    SysBusDevice busdev;
    uint32_t vcram_base, vcram_size;

    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem;
    MemoryRegionSection fbsection;

    QemuConsole *con;
    bool lock, invalidate;

    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bpp;
    uint32_t base, pitch, size;
    uint32_t pixo, alpha;

    int pending;
    qemu_irq mbox_irq;
} Bcm2835FbState;

void bcm2835_fb_reconfigure(Bcm2835FbState *s, uint32_t *xres, uint32_t *yres,
                            uint32_t *xoffset, uint32_t *yoffset, uint32_t *bpp,
                            uint32_t *pixo, uint32_t *alpha);

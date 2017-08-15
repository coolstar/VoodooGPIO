#include "VoodooGPIO.h"

OSDefineMetaClassAndStructors(VoodooGPIO, IOService);

#if defined(__LP64__) && __LP64__
#define BITS_PER_LONG 64
#else
#define BITS_PER_LONG 32
#endif

#define BIT(x) 1UL << x
#define GENMASK(h, l) \
(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

/* Offset from regs */
#define REVID				0x000
#define REVID_SHIFT			16
#define REVID_MASK			GENMASK(31, 16)

#define PADBAR				0x00c
#define GPI_IS				0x100
#define GPI_GPE_STS			0x140
#define GPI_GPE_EN			0x160

#define PADOWN_BITS			4
#define PADOWN_SHIFT(p)			((p) % 8 * PADOWN_BITS)
#define PADOWN_MASK(p)			(0xf << PADOWN_SHIFT(p))
#define PADOWN_GPP(p)			((p) / 8)

/* Offset from pad_regs */
#define PADCFG0				0x000
#define PADCFG0_RXEVCFG_SHIFT		25
#define PADCFG0_RXEVCFG_MASK		(3 << PADCFG0_RXEVCFG_SHIFT)
#define PADCFG0_RXEVCFG_LEVEL		0
#define PADCFG0_RXEVCFG_EDGE		1
#define PADCFG0_RXEVCFG_DISABLED	2
#define PADCFG0_RXEVCFG_EDGE_BOTH	3
#define PADCFG0_PREGFRXSEL		BIT(24)
#define PADCFG0_RXINV			BIT(23)
#define PADCFG0_GPIROUTIOXAPIC		BIT(20)
#define PADCFG0_GPIROUTSCI		BIT(19)
#define PADCFG0_GPIROUTSMI		BIT(18)
#define PADCFG0_GPIROUTNMI		BIT(17)
#define PADCFG0_PMODE_SHIFT		10
#define PADCFG0_PMODE_MASK		(0xf << PADCFG0_PMODE_SHIFT)
#define PADCFG0_GPIORXDIS		BIT(9)
#define PADCFG0_GPIOTXDIS		BIT(8)
#define PADCFG0_GPIORXSTATE		BIT(1)
#define PADCFG0_GPIOTXSTATE		BIT(0)

#define PADCFG1				0x004
#define PADCFG1_TERM_UP			BIT(13)
#define PADCFG1_TERM_SHIFT		10
#define PADCFG1_TERM_MASK		(7 << PADCFG1_TERM_SHIFT)
#define PADCFG1_TERM_20K		4
#define PADCFG1_TERM_2K			3
#define PADCFG1_TERM_5K			2
#define PADCFG1_TERM_1K			1

#define PADCFG2				0x008
#define PADCFG2_DEBEN			BIT(0)
#define PADCFG2_DEBOUNCE_SHIFT		1
#define PADCFG2_DEBOUNCE_MASK		GENMASK(4, 1)

#define DEBOUNCE_PERIOD			31250 /* ns */

#define pin_to_padno(c, p)	((p) - (c)->pin_base)
#define padgroup_offset(g, p)	((p) - (g)->base)

UInt32 VoodooGPIO::readl(IOVirtualAddress addr) {
    return *(const volatile UInt32 *)addr;
}

void VoodooGPIO::writel(UInt32 b, IOVirtualAddress addr){
    *(volatile UInt32 *)(addr) = b;
}

struct intel_community *VoodooGPIO::intel_get_community(unsigned pin){
    struct intel_community *community;
    for (int i = 0; i < ncommunities; i++){
        community = &communities[i];
        if (pin >= community->pin_base && pin < community->pin_base + community->npins)
            return community;
    }
    
    IOLog("%s::Failed to find community for pin %u", getName(), pin);
    return NULL;
}

const struct intel_padgroup *VoodooGPIO::intel_community_get_padgroup(const struct intel_community *community, unsigned pin){
    for (int i = 0; i < community->ngpps; i++){
        const struct intel_padgroup *padgrp = &community->gpps[i];
        if (pin > padgrp->base && pin < padgrp->base + padgrp->size)
            return padgrp;
    }
    
    IOLog("%s::Failed to find padgroup for pin %u", getName(), pin);
    return NULL;
}

IOVirtualAddress VoodooGPIO::intel_get_padcfg(unsigned pin, unsigned reg){
    const struct intel_community *community;
    unsigned padno;
    size_t nregs;
    
    community = intel_get_community(pin);
    if (!community)
        return NULL;
    
    padno = pin_to_padno(community, pin);
    nregs = (community->features & PINCTRL_FEATURE_DEBOUNCE) ? 4 : 2;
    
    if (reg == PADCFG2 && !(community->features & PINCTRL_FEATURE_DEBOUNCE))
        return NULL;
    
    return community->pad_regs + reg + padno * nregs * 4;
}

bool VoodooGPIO::intel_pad_owned_by_host(unsigned pin){
    const struct intel_community *community;
    const struct intel_padgroup *padgrp;
    unsigned gpp, offset, gpp_offset;
    IOVirtualAddress padown;
    
    community = intel_get_community(pin);
    if (!community)
        return false;
    if (!community->padown_offset)
        return true;
    
    padgrp = intel_community_get_padgroup(community, pin);
    if (!padgrp)
        return false;
    
    gpp_offset = padgroup_offset(padgrp, pin);
    gpp = PADOWN_GPP(gpp_offset);
    
    offset = community->padown_offset + padgrp->padown_num * 4 + gpp * 4;
    padown = community->regs + offset;
    
    return !(readl(padown) & PADOWN_MASK(gpp_offset));
}

bool VoodooGPIO::intel_pad_acpi_mode(unsigned pin){
    const struct intel_community *community;
    const struct intel_padgroup *padgrp;
    unsigned offset, gpp_offset;
    IOVirtualAddress hostown;
    
    community = intel_get_community(pin);
    if (!community)
        return true;
    if (!community->hostown_offset)
        return false;
    
    padgrp = intel_community_get_padgroup(community, pin);
    if (!padgrp)
        return true;
    
    gpp_offset = padgroup_offset(padgrp, pin);
    offset = community->hostown_offset + padgrp->reg_num * 4;
    hostown = community->regs + offset;
    
    return !(readl(hostown) & BIT(gpp_offset));
}

bool VoodooGPIO::intel_pad_locked(unsigned pin){
    const struct intel_community *community;
    const struct intel_padgroup *padgrp;
    unsigned offset, gpp_offset;
    UInt32 value;
    
    community = intel_get_community(pin);
    if (!community)
        return true;
    if (!community->padcfglock_offset)
        return false;
    
    padgrp = intel_community_get_padgroup(community, pin);
    if (!padgrp)
        return true;
    
    gpp_offset = padgroup_offset(padgrp, pin);
    
    /*
     * If PADCFGLOCK and PADCFGLOCKTX bits are both clear for this pad,
     * the pad is considered unlocked. Any other case means that it is
     * either fully or partially locked and we don't touch it.
     */
    offset = community->padcfglock_offset + padgrp->reg_num * 8;
    value = readl(community->regs + offset);
    if (value & BIT(gpp_offset))
        return true;
    
    offset = community->padcfglock_offset + 4 + padgrp->reg_num * 8;
    value = readl(community->regs + offset);
    if (value & BIT(gpp_offset))
        return true;
    
    return false;
}

void VoodooGPIO::intel_gpio_irq_enable(unsigned pin){
    const struct intel_community *community;
    community = intel_get_community(pin);
    if (community){
        const struct intel_padgroup *padgrp;
        unsigned gpp, gpp_offset;
        UInt32 value;
        
        padgrp = intel_community_get_padgroup(community, pin);
        if (!padgrp)
            return;
        
        gpp = padgrp->reg_num;
        gpp_offset = padgroup_offset(padgrp, pin);
        /* Clear interrupt status first to avoid unexpected interrupt */
        writel(BIT(gpp_offset), community->regs + GPI_IS + gpp * 4);
        
        value = readl(community->regs + community->ie_offset + gpp * 4);
        value |= BIT(gpp_offset);
        writel(value, community->regs + community->ie_offset + gpp * 4);
    }
}

void VoodooGPIO::intel_gpio_irq_mask_unmask(unsigned pin, bool mask)
{
    const struct intel_community *community;
    
    community = intel_get_community(pin);
    if (community) {
        const struct intel_padgroup *padgrp;
        unsigned gpp, gpp_offset;
        IOVirtualAddress reg;
        UInt32 value;
        
        padgrp = intel_community_get_padgroup(community, pin);
        if (!padgrp)
            return;
        
        gpp = padgrp->reg_num;
        gpp_offset = padgroup_offset(padgrp, pin);
        
        reg = community->regs + community->ie_offset + gpp * 4;
        
        value = readl(reg);
        if (mask)
            value &= ~BIT(gpp_offset);
        else
            value |= BIT(gpp_offset);
        writel(value, reg);
    }
}

bool VoodooGPIO::intel_gpio_irq_set_type(unsigned pin, unsigned type){
    IOVirtualAddress reg;
    UInt32 value;
    
    reg = intel_get_padcfg(pin, PADCFG0);
    if (!reg)
        return false;
    
    /*
     * If the pin is in ACPI mode it is still usable as a GPIO but it
     * cannot be used as IRQ because GPI_IS status bit will not be
     * updated by the host controller hardware.
     */
    if (intel_pad_acpi_mode(pin)) {
        IOLog("%s:: pin %u cannot be used as IRQ\n", getName(), pin);
        return false;
    }
    
    value = readl(reg);
    
    value &= ~(PADCFG0_RXEVCFG_MASK | PADCFG0_RXINV);
    
    if ((type & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH) {
        value |= PADCFG0_RXEVCFG_EDGE_BOTH << PADCFG0_RXEVCFG_SHIFT;
    } else if (type & IRQ_TYPE_EDGE_FALLING) {
        value |= PADCFG0_RXEVCFG_EDGE << PADCFG0_RXEVCFG_SHIFT;
        value |= PADCFG0_RXINV;
    } else if (type & IRQ_TYPE_EDGE_RISING) {
        value |= PADCFG0_RXEVCFG_EDGE << PADCFG0_RXEVCFG_SHIFT;
    } else if (type & IRQ_TYPE_LEVEL_MASK) {
        if (type & IRQ_TYPE_LEVEL_LOW)
            value |= PADCFG0_RXINV;
    } else {
        value |= PADCFG0_RXEVCFG_DISABLED << PADCFG0_RXEVCFG_SHIFT;
    }
    
    writel(value, reg);
    return true;
}

bool VoodooGPIO::intel_pinctrl_add_padgroups(intel_community *community){
    struct intel_padgroup *gpps;
    unsigned padown_num = 0;
    size_t ngpps;
    
    if (community->gpps)
        ngpps = community->ngpps;
    else
        ngpps = DIV_ROUND_UP(community->npins, community->gpp_size);
    
    gpps = (struct intel_padgroup *)IOMalloc(ngpps * sizeof(struct intel_padgroup));
    
    for (int i = 0; i < ngpps; i++){
        if (community->gpps){
            gpps[i] = community->gpps[i];
        } else {
            unsigned gpp_size = community->gpp_size;
            gpps[i].reg_num = i;
            gpps[i].base = community->pin_base + i * gpp_size;
            gpps[i].size = min(gpp_size, npins);
            npins -= gpps[i].size;
        }
        
        if (gpps[i].size > 32){
            IOLog("%s::Invalid GPP size for pad group %d\n", getName(), i);
            return false;
        }
        
        gpps[i].padown_num = padown_num;
        
        /*
         * In older hardware the number of padown registers per
         * group is fixed regardless of the group size.
         */
        if (community->gpp_num_padown_regs)
            padown_num += community->gpp_num_padown_regs;
        else
            padown_num += DIV_ROUND_UP(gpps[i].size * 4, 32);
    }
    community->gpps = gpps;
    community->ngpps = ngpps;
    community->gpps_alloc = true;
    return true;
}

bool VoodooGPIO::start(IOService *provider){
    if (!npins || !ngroups || !nfunctions || !ncommunities){
        IOLog("%s::Missing Platform Data! Aborting!\n", getName());
        return false;
    }
    
    if (!IOService::start(provider))
        return false;
    
    workLoop = getWorkLoop();
    if (!workLoop){
        IOLog("%s::Failed to get workloop!\n", getName());
        stop(provider);
        return false;
    }
    workLoop->retain();
    
    interruptSource = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &VoodooGPIO::InterruptOccurred), provider);
    if (!interruptSource){
        IOLog("%s::Failed to get GPIO Controller interrupt!\n", getName());
        stop(provider);
        return false;
    }
    
    workLoop->addEventSource(interruptSource);
    
    interruptSource->enable();
    
    IOLog("%s::VoodooGPIO Init!\n", getName());
    
    for (int i = 0; i < ncommunities; i++){
        IOLog("%s::VoodooGPIO Initializing Community %d\n", getName(), i);
        intel_community *community = &communities[i];
        
        community->regs = 0;
        community->pad_regs = 0;
        
        community->mmap = provider->mapDeviceMemoryWithIndex(i);
        if (!community->mmap){
            IOLog("%s:VoodooGPIO error mapping community %d\n", getName(), i);
            continue;
        }
        
        IOVirtualAddress regs = community->mmap->getVirtualAddress();
        community->regs = regs;
        
        /*
         * Determine community features based on the revision if
         * not specified already.
         */
        if (!community->features){
            UInt32 rev;
            rev = (readl(regs + REVID) & REVID_MASK) >> REVID_SHIFT;
            if (rev >= 0x94){
                community->features |= PINCTRL_FEATURE_DEBOUNCE;
                community->features |= PINCTRL_FEATURE_1K_PD;
            }
        }
        
        /* Read offset of the pad configuration registers */
        UInt32 padbar = readl(regs + PADBAR);
        
        community->pad_regs = regs + padbar;
        
        if (!intel_pinctrl_add_padgroups(community)){
            IOLog("%s::Error adding padgroups to community %d\n", getName(), i);
        }
    }
    
    //0x1B is ELAN0651 on Yoga 720
    intel_gpio_irq_set_type(0x1B, IRQ_TYPE_LEVEL_LOW);
    intel_gpio_irq_enable(0x1B);
    
    registerService();
    
    return true;
}

void VoodooGPIO::stop(IOService *provider){
    IOLog("%s::VoodooGPIO stop!\n", getName());
    
    intel_gpio_irq_mask_unmask(0x1B, true);
    
    for (int i = 0; i < ncommunities; i++){
        if (communities[i].gpps_alloc){
            IOFree((void *)communities[i].gpps, communities[i].ngpps * sizeof(struct intel_padgroup));
            communities[i].gpps = NULL;
        }
    }
    
    if (interruptSource){
        workLoop->removeEventSource(interruptSource);
        interruptSource->disable();
        interruptSource = NULL;
    }
    
    if (workLoop){
        workLoop->release();
        workLoop = NULL;
    }
    
    IOService::stop(provider);
}

void VoodooGPIO::intel_gpio_community_irq_handler(struct intel_community *community){
    for (int gpp = 0; gpp < community->ngpps; gpp++){
        const struct intel_padgroup *padgrp = &community->gpps[gpp];
        
        unsigned long pending, enabled, gpp_offset;
        
        pending = readl(community->regs + GPI_IS + padgrp->reg_num * 4);
        enabled = readl(community->regs + community->ie_offset +
                        padgrp->reg_num * 4);
        
        /* Only interrupts that are enabled */
        pending &= enabled;
        
        unsigned padno = padgrp->base - community->pin_base;
        if (padno >= community->npins)
            break;
        
        for (int i = 0; i < 32; i++){
            bool isPin = (pending >> i) & 0x1;
            if (isPin){
                unsigned pin = padno + i;
                IOLog("%s::Interrupt on pin %u\n", getName(), pin);
            }
        }
    }
}

void VoodooGPIO::InterruptOccurred(OSObject *owner, IOInterruptEventSource *src, int intCount){
    //IOLog("%s::VoodooGPIO got an interrupt!\n", getName());
    for (int i = 0; i < ncommunities; i++){
        struct intel_community *community = &communities[i];
        intel_gpio_community_irq_handler(community);
    }
}

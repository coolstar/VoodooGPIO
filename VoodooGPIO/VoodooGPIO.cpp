//
//  VoodooGPIO.cpp
//  VoodooGPIO
//
//  Created by CoolStar on 8/14/17.
//  Copyright © 2017 CoolStar. All rights reserved.
//

#include "VoodooGPIO.hpp"

OSDefineMetaClassAndStructors(VoodooGPIO, IOService);

#define kIOPMPowerOff 0

#if defined(__LP64__) && __LP64__
#define BITS_PER_LONG 64
#else
#define BITS_PER_LONG 32
#endif

#define BIT(x) 1UL << x
#define GENMASK(h, l) \
(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* Offset from regs */
#define REVID               0x000
#define REVID_SHIFT         16
#define REVID_MASK          GENMASK(31, 16)

#define PADBAR              0x00c
#define GPI_IS              0x100

#define PADOWN_BITS         4
#define PADOWN_SHIFT(p)     ((p) % 8 * PADOWN_BITS)
#define PADOWN_MASK(p)      (0xf << PADOWN_SHIFT(p))
#define PADOWN_GPP(p)       ((p) / 8)

/* Offset from pad_regs */
#define PADCFG0                     0x000
#define PADCFG0_RXEVCFG_SHIFT       25
#define PADCFG0_RXEVCFG_MASK        (3 << PADCFG0_RXEVCFG_SHIFT)
#define PADCFG0_RXEVCFG_LEVEL       0
#define PADCFG0_RXEVCFG_EDGE        1
#define PADCFG0_RXEVCFG_DISABLED    2
#define PADCFG0_RXEVCFG_EDGE_BOTH   3
#define PADCFG0_PREGFRXSEL          BIT(24)
#define PADCFG0_RXINV               BIT(23)
#define PADCFG0_GPIROUTIOXAPIC      BIT(20)
#define PADCFG0_GPIROUTSCI          BIT(19)
#define PADCFG0_GPIROUTSMI          BIT(18)
#define PADCFG0_GPIROUTNMI          BIT(17)
#define PADCFG0_PMODE_SHIFT         10
#define PADCFG0_PMODE_MASK          (0xf << PADCFG0_PMODE_SHIFT)
#define PADCFG0_GPIORXDIS           BIT(9)
#define PADCFG0_GPIOTXDIS           BIT(8)
#define PADCFG0_GPIORXSTATE         BIT(1)
#define PADCFG0_GPIOTXSTATE         BIT(0)

#define PADCFG1                     0x004
#define PADCFG1_TERM_UP             BIT(13)
#define PADCFG1_TERM_SHIFT          10
#define PADCFG1_TERM_MASK           (7 << PADCFG1_TERM_SHIFT)
#define PADCFG1_TERM_20K            4
#define PADCFG1_TERM_2K             3
#define PADCFG1_TERM_5K             2
#define PADCFG1_TERM_1K             1

#define PADCFG2                     0x008
#define PADCFG2_DEBEN               BIT(0)
#define PADCFG2_DEBOUNCE_SHIFT      1
#define PADCFG2_DEBOUNCE_MASK       GENMASK(4, 1)

#define DEBOUNCE_PERIOD             31250 /* ns */

#define pin_to_padno(c, p)      ((p) - (c)->pin_base)
#define padgroup_offset(g, p)   ((p) - (g)->base)

UInt32 VoodooGPIO::readl(IOVirtualAddress addr) {
    return *(const volatile UInt32 *)addr;
}

void VoodooGPIO::writel(UInt32 b, IOVirtualAddress addr) {
    *(volatile UInt32 *)(addr) = b;
}

IOWorkLoop* VoodooGPIO::getWorkLoop() {
    // Do we have a work loop already?, if so return it NOW.
    if ((vm_address_t) workLoop >> 1)
        return workLoop;

    if (OSCompareAndSwap(0, 1, reinterpret_cast<IOWorkLoop*>(&workLoop))) {
        // Construct the workloop and set the cntrlSync variable
        // to whatever the result is and return
        workLoop = IOWorkLoop::workLoop();
    } else {
        while (reinterpret_cast<IOWorkLoop*>(workLoop) == reinterpret_cast<IOWorkLoop*>(1)) {
            // Spin around the cntrlSync variable until the
            // initialization finishes.
            thread_block(0);
        }
    }

    return workLoop;
}

struct intel_community *VoodooGPIO::intel_get_community(unsigned pin) {
    struct intel_community *community;
    for (int i = 0; i < ncommunities; i++) {
        community = &communities[i];
        if (pin >= community->pin_base && pin < community->pin_base + community->npins)
            return community;
    }

    IOLog("%s::Failed to find community for pin %u", getName(), pin);
    return NULL;
}

const struct intel_padgroup *VoodooGPIO::intel_community_get_padgroup(const struct intel_community *community, unsigned pin) {
    for (int i = 0; i < community->ngpps; i++) {
        const struct intel_padgroup *padgrp = &community->gpps[i];
        if (pin >= padgrp->base && pin < padgrp->base + padgrp->size)
            return padgrp;
    }

    IOLog("%s::Failed to find padgroup for pin %u", getName(), pin);
    return NULL;
}

IOVirtualAddress VoodooGPIO::intel_get_padcfg(unsigned pin, unsigned reg) {
    const struct intel_community *community;
    unsigned padno;
    size_t nregs;
    
    community = intel_get_community(pin);
    if (!community)
        return 0;
    
    padno = pin_to_padno(community, pin);
    nregs = (community->features & PINCTRL_FEATURE_DEBOUNCE) ? 4 : 2;
    
    if (reg == PADCFG2 && !(community->features & PINCTRL_FEATURE_DEBOUNCE))
        return 0;
    
    return community->pad_regs + reg + padno * nregs * 4;
}

bool VoodooGPIO::intel_pad_owned_by_host(unsigned pin) {
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

bool VoodooGPIO::intel_pad_acpi_mode(unsigned pin) {
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

bool VoodooGPIO::intel_pad_locked(unsigned pin) {
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

/**
 * Translate GPIO offset to hardware pin (They are not always the same).
 * Putting appropriate community and padgroup in the variables.
 *
 * @param offset GPIO pin number.
 * @param community Matching community for hardware pin number.
 * @param padgrp Matching padgroup for hardware pin number.
 * @return Hardware GPIO pin number. -1 if not found.
 */
SInt32 VoodooGPIO::intel_gpio_to_pin(UInt32 offset,
                                  const struct intel_community **community,
                                  const struct intel_padgroup **padgrp) {
    int i;
    for (i = 0; i < ncommunities; i++) {
        const struct intel_community *comm = &communities[i];
        int j;
        for (j = 0; j < comm->ngpps; j++) {
            const struct intel_padgroup *pgrp = &comm->gpps[j];
            if (pgrp->gpio_base < 0)
                continue;

            if (offset >= pgrp->gpio_base &&
                offset < pgrp->gpio_base + pgrp->size) {
                int pin;

                pin = pgrp->base + offset - pgrp->gpio_base;
                if (community)
                    *community = comm;
                if (padgrp)
                    *padgrp = pgrp;
                return pin;
            }
        }
    }

    IOLog("%s::Failed getting hardware pin for GPIO pin %u", getName(), offset);
    return -1;
}

/**
 * @param pin Hardware GPIO pin number to enable.
 */
void VoodooGPIO::intel_gpio_irq_enable(UInt32 pin) {
    const struct intel_community *community = intel_get_community(pin);
    if (community) {
        const struct intel_padgroup *padgrp = intel_community_get_padgroup(community, pin);
        if (!padgrp)
            return;

        UInt32 gpp, gpp_offset;
        UInt32 value;

        gpp = padgrp->reg_num;
        gpp_offset = padgroup_offset(padgrp, pin);
        /* Clear interrupt status first to avoid unexpected interrupt */
        writel(BIT(gpp_offset), community->regs + GPI_IS + gpp * 4);

        value = readl(community->regs + community->ie_offset + gpp * 4);
        value |= BIT(gpp_offset);
        writel(value, community->regs + community->ie_offset + gpp * 4);
    }
}

/**
 * @param pin Hardware GPIO pin number to mask.
 * @param mask Whether to mask or unmask.
 */
void VoodooGPIO::intel_gpio_irq_mask_unmask(unsigned pin, bool mask) {
    const struct intel_community *community = intel_get_community(pin);
    if (community) {
        const struct intel_padgroup *padgrp = intel_community_get_padgroup(community, pin);
        if (!padgrp)
            return;

        unsigned gpp, gpp_offset;
        IOVirtualAddress reg;
        UInt32 value;

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

/**
 * @param pin Hardware GPIO pin number to set its type.
 * @param type Type to set.
 */
bool VoodooGPIO::intel_gpio_irq_set_type(unsigned pin, unsigned type) {
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

bool VoodooGPIO::intel_pinctrl_add_padgroups(intel_community *community) {
    struct intel_padgroup *gpps;
    unsigned padown_num = 0;
    size_t ngpps;
    
    if (community->gpps)
        ngpps = community->ngpps;
    else
        ngpps = DIV_ROUND_UP(community->npins, community->gpp_size);
    
    gpps = (struct intel_padgroup *)IOMalloc(ngpps * sizeof(struct intel_padgroup));
    
    for (int i = 0; i < ngpps; i++) {
        if (community->gpps) {
            gpps[i] = community->gpps[i];
        } else {
            unsigned gpp_size = community->gpp_size;
            gpps[i].reg_num = i;
            gpps[i].base = community->pin_base + i * gpp_size;
            gpps[i].size = min(gpp_size, npins);
            gpps[i].gpio_base = 0;
            npins -= gpps[i].size;
        }

        if (gpps[i].size > 32) {
            IOLog("%s::Invalid GPP size for pad group %d\n", getName(), i);
            return false;
        }

        // Set gpio_base to base if not specified
        if (!gpps[i].gpio_base)
            gpps[i].gpio_base = gpps[i].base;

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

bool VoodooGPIO::intel_pinctrl_should_save(unsigned pin) {
    if (!(intel_pad_owned_by_host(pin) && !intel_pad_locked(pin)))
        return false;
    
    struct intel_community *community = intel_get_community(pin);
    
    unsigned communityidx = pin - community->pin_base;

    /*
     * Only restore the pin if it is actually in use by the kernel (or
     * by userspace). It is possible that some pins are used by the
     * BIOS during resume and those are not always locked down so leave
     * them alone.
     */
    if (community->pinInterruptActionOwners[communityidx])
        return true;
    return false;
}

void VoodooGPIO::intel_pinctrl_pm_init() {
    context.pads = (struct intel_pad_context *)IOMalloc(npins * sizeof(struct intel_pad_context));
    memset(context.pads, 0, npins * sizeof(struct intel_pad_context));
    
    context.communities = (struct intel_community_context *)IOMalloc(ncommunities * sizeof(struct intel_community_context));
    memset(context.communities, 0, ncommunities * sizeof(struct intel_community_context));
    
    for (int i = 0; i < ncommunities; i++) {
        struct intel_community *community = &communities[i];
        UInt32 *intmask = (UInt32 *)IOMalloc(community->ngpps * sizeof(UInt32));
        
        context.communities[i].intmask = intmask;
    }
}

void VoodooGPIO::intel_pinctrl_pm_release() {
    for (int i = 0; i < ncommunities; i++) {
        struct intel_community *community = &communities[i];
        IOFree(context.communities[i].intmask, community->ngpps * sizeof(UInt32));
        
        context.communities[i].intmask = NULL;
    }
    
    IOFree(context.communities, ncommunities * sizeof(struct intel_community_context));
    context.communities = NULL;
    
    IOFree(context.pads, npins * sizeof(intel_pad_context));
    context.pads = NULL;
}

void VoodooGPIO::intel_pinctrl_suspend() {
    struct intel_pad_context *pads = context.pads;
    for (int i = 0; i < npins; i++) {
        const struct pinctrl_pin_desc *desc = &pins[i];
        IOVirtualAddress padcfg;
        uint32_t val;
        
        if (!intel_pinctrl_should_save(desc->number))
            continue;
        
        val = readl(intel_get_padcfg(desc->number, PADCFG0));
        pads[i].padcfg0 = val & ~PADCFG0_GPIORXSTATE;
        val = readl(intel_get_padcfg(desc->number, PADCFG1));
        pads[i].padcfg1 = val;
        
        padcfg = intel_get_padcfg(desc->number, PADCFG2);
        if (padcfg)
            pads[i].padcfg2 = readl(padcfg);
    }
    
    struct intel_community_context *communityContexts = context.communities;
    for (int i = 0; i < ncommunities; i++) {
        struct intel_community *community = &communities[i];
        
        IOVirtualAddress base = community->regs + community->ie_offset;
        
        for (unsigned gpp = 0; gpp < community->ngpps; gpp++)
            communityContexts[i].intmask[gpp] = readl(base + gpp * 4);
    }
}

void VoodooGPIO::intel_gpio_irq_init() {
    for (size_t i = 0; i < ncommunities; i++) {
        struct intel_community *community = &communities[i];
        IOVirtualAddress base = community->regs;
        
        for (unsigned gpp = 0; gpp < community->ngpps; gpp++) {
            /* Mask and clear all interrupts */
            writel(0, base + community->ie_offset + gpp * 4);
            writel(0xffff, base + GPI_IS + gpp * 4);
        }
    }
}

void VoodooGPIO::intel_pinctrl_resume() {
    /* Mask all interrupts */
    intel_gpio_irq_init();
    
    struct intel_pad_context *pads = context.pads;
    for (int i = 0; i < npins; i++) {
        const struct pinctrl_pin_desc *desc = &pins[i];
        IOVirtualAddress padcfg;
        uint32_t val;
        
        if (!intel_pinctrl_should_save(desc->number))
            continue;
        
        padcfg = intel_get_padcfg(desc->number, PADCFG0);
        val = readl(padcfg) & ~PADCFG0_GPIORXSTATE;
        if (val != pads[i].padcfg0) {
            writel(pads[i].padcfg0, padcfg);
        }
        
        padcfg = intel_get_padcfg(desc->number, PADCFG1);
        val = readl(padcfg);
        if (val != pads[i].padcfg1) {
            writel(pads[i].padcfg1, padcfg);
        }
        
        padcfg = intel_get_padcfg(desc->number, PADCFG2);
        if (padcfg) {
            val = readl(padcfg);
            if (val != pads[i].padcfg2) {
                writel(pads[i].padcfg2, padcfg);
            }
        }
    }
    
    struct intel_community_context *communityContexts = context.communities;
    for (int i = 0; i < ncommunities; i++) {
        struct intel_community *community = &communities[i];
        
        IOVirtualAddress base = communities->regs + communities->ie_offset;
        
        for (unsigned gpp = 0; gpp < community->ngpps; gpp++) {
            writel(communityContexts[i].intmask[gpp], base + gpp * 4);
        }
    }
}

bool VoodooGPIO::start(IOService *provider) {
    if (!npins || !ngroups || !nfunctions || !ncommunities) {
        IOLog("%s::Missing Platform Data! Aborting!\n", getName());
        return false;
    }
    
    if (!IOService::start(provider))
        return false;
    
    PMinit();
    
    workLoop = getWorkLoop();
    if (!workLoop) {
        IOLog("%s::Failed to get workloop!\n", getName());
        stop(provider);
        return false;
    }
    workLoop->retain();
    
    interruptSource = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &VoodooGPIO::InterruptOccurred), provider);
    if (!interruptSource) {
        IOLog("%s::Failed to get GPIO Controller interrupt!\n", getName());
        stop(provider);
        return false;
    }
    
    workLoop->addEventSource(interruptSource);
    interruptSource->enable();
    
    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (workLoop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s Could not open command gate\n", getName());
        stop(provider);
        return false;
    }

    IOLog("%s::VoodooGPIO Init!\n", getName());
    
    for (int i = 0; i < ncommunities; i++) {
        IOLog("%s::VoodooGPIO Initializing Community %d\n", getName(), i);
        intel_community *community = &communities[i];
        
        community->regs = 0;
        community->pad_regs = 0;
        
        community->mmap = provider->mapDeviceMemoryWithIndex(i);
        if (!community->mmap) {
            IOLog("%s:VoodooGPIO error mapping community %d\n", getName(), i);
            continue;
        }
        
        IOVirtualAddress regs = community->mmap->getVirtualAddress();
        community->regs = regs;
        
        /*
         * Determine community features based on the revision if
         * not specified already.
         */
        if (!community->features) {
            UInt32 rev;
            rev = (readl(regs + REVID) & REVID_MASK) >> REVID_SHIFT;
            if (rev >= 0x94) {
                community->features |= PINCTRL_FEATURE_DEBOUNCE;
                community->features |= PINCTRL_FEATURE_1K_PD;
            }
        }
        
        /* Read offset of the pad configuration registers */
        UInt32 padbar = readl(regs + PADBAR);
        
        community->pad_regs = regs + padbar;
        
        if (!intel_pinctrl_add_padgroups(community)) {
            IOLog("%s::Error adding padgroups to community %d\n", getName(), i);
        }
    }
    
    for (int i = 0; i < ncommunities; i++) {
        size_t sz = sizeof(OSObject *) * communities[i].npins;
        communities[i].pinInterruptActionOwners = (OSObject **)IOMalloc(sz);
        memset(communities[i].pinInterruptActionOwners, 0, sz);
        
        sz = sizeof(IOInterruptAction) * communities[i].npins;
        communities[i].pinInterruptAction = (IOInterruptAction *)IOMalloc(sz);
        memset(communities[i].pinInterruptAction, 0, sz);
        
        sz = sizeof(unsigned) * communities[i].npins;
        communities[i].interruptTypes = (unsigned *)IOMalloc(sz);
        memset(communities[i].interruptTypes, 0, sz);
        
        sz = sizeof(void *) * communities[i].npins;
        communities[i].pinInterruptRefcons = (void **)IOMalloc(sz);
        memset(communities[i].pinInterruptRefcons, 0, sz);
    }
    
    intel_pinctrl_pm_init();
    
    controllerIsAwake = true;
    
    registerService();
    
    // Declare an array of two IOPMPowerState structures (kMyNumberOfStates = 2).
    
#define kMyNumberOfStates 2
    
    static IOPMPowerState myPowerStates[kMyNumberOfStates];
    // Zero-fill the structures.
    bzero (myPowerStates, sizeof(myPowerStates));
    // Fill in the information about your device's off state:
    myPowerStates[0].version = 1;
    myPowerStates[0].capabilityFlags = kIOPMPowerOff;
    myPowerStates[0].outputPowerCharacter = kIOPMPowerOff;
    myPowerStates[0].inputPowerRequirement = kIOPMPowerOff;
    // Fill in the information about your device's on state:
    myPowerStates[1].version = 1;
    myPowerStates[1].capabilityFlags = kIOPMPowerOn;
    myPowerStates[1].outputPowerCharacter = kIOPMPowerOn;
    myPowerStates[1].inputPowerRequirement = kIOPMPowerOn;
    
    provider->joinPMtree(this);
    
    registerPowerDriver(this, myPowerStates, kMyNumberOfStates);
    
    return true;
}

void VoodooGPIO::stop(IOService *provider) {
    IOLog("%s::VoodooGPIO stop!\n", getName());

    intel_pinctrl_pm_release();
    
    for (int i = 0; i < ncommunities; i++) {
        if (communities[i].gpps_alloc) {
            IOFree((void *)communities[i].gpps, communities[i].ngpps * sizeof(struct intel_padgroup));
            communities[i].gpps = NULL;
        }
        
        for (int j = 0; j < communities[i].npins; j++) {
            communities[i].pinInterruptActionOwners[j] = NULL;
            communities[i].pinInterruptAction[j] = NULL;
            communities[i].interruptTypes[j] = 0;
            communities[i].pinInterruptRefcons[j] = NULL;
        }
        IOFree(communities[i].pinInterruptActionOwners, sizeof(OSObject *) * communities[i].npins);
        IOFree(communities[i].pinInterruptAction, sizeof(IOInterruptAction) * communities[i].npins);
        IOFree(communities[i].interruptTypes, sizeof(unsigned) * communities[i].npins);
        IOFree(communities[i].pinInterruptRefcons, sizeof(void *) * communities[i].npins);
    }
    
    if (interruptSource) {
        interruptSource->disable();
        workLoop->removeEventSource(interruptSource);
        OSSafeReleaseNULL(interruptSource);
    }
    
    if (command_gate) {
        workLoop->removeEventSource(command_gate);
        OSSafeReleaseNULL(command_gate);
    }
    
    if (workLoop) {
        workLoop->release();
        workLoop = NULL;
    }
    
    PMstop();
    
    IOService::stop(provider);
}

IOReturn VoodooGPIO::setPowerState(unsigned long powerState, IOService *whatDevice) {
    if (powerState == 0) {
        controllerIsAwake = false;
        
        for (int i = 0; i < ncommunities; i++) {
            struct intel_community *community = &communities[i];
            for (int j = 0; j < community->npins; j++) {
                OSObject *pinInterruptOwner = community->pinInterruptActionOwners[j];
                if (pinInterruptOwner) {
                    int pin = community->pin_base + j;
                    intel_gpio_irq_mask_unmask(pin, true);
                }
            }
        }
        
        IOLog("%s::Going to Sleep!\n", getName());
    } else {
        if (!controllerIsAwake) {
            controllerIsAwake = true;
            
            for (int i = 0; i < ncommunities; i++) {
                struct intel_community *community = &communities[i];
                for (int j = 0; j < community->npins; j++) {
                    OSObject *pinInterruptOwner = community->pinInterruptActionOwners[j];
                    if (pinInterruptOwner) {
                        int pin = community->pin_base + j;
                        intel_gpio_irq_set_type(pin, community->interruptTypes[j]);
                        intel_gpio_irq_enable(pin);
                    }
                }
            }
            
            IOLog("%s::Woke up from Sleep!\n", getName());
        } else {
            IOLog("%s::GPIO Controller is already awake! Not reinitializing.\n", getName());
        }
    }
    return kIOPMAckImplied;
}

void VoodooGPIO::intel_gpio_community_irq_handler(struct intel_community *community) {
    for (int gpp = 0; gpp < community->ngpps; gpp++) {
        const struct intel_padgroup *padgrp = &community->gpps[gpp];
        
        unsigned long pending, enabled;
        
        pending = readl(community->regs + GPI_IS + padgrp->reg_num * 4);
        enabled = readl(community->regs + community->ie_offset +
                        padgrp->reg_num * 4);
        
        /* Only interrupts that are enabled */
        pending &= enabled;
        
        unsigned padno = padgrp->base - community->pin_base;
        if (padno >= community->npins)
            break;
        
        for (int i = 0; i < 32; i++) {
            bool isPin = (pending >> i) & 0x1;
            if (isPin) {
                unsigned pin = padno + i;
                
                OSObject *owner = community->pinInterruptActionOwners[pin];
                if (owner) {
                    IOInterruptAction handler = community->pinInterruptAction[pin];
                    void *refcon = community->pinInterruptRefcons[pin];
                    handler(owner, refcon, this, pin);
                }
                
                if (community->interruptTypes[pin] & IRQ_TYPE_LEVEL_MASK)
                    intel_gpio_irq_enable(community->pin_base + pin); // For Level interrupts, we need to clear the interrupt status or we get too many interrupts
            }
        }
    }
}

/**
 * @param pin 'Software' pin number (i.e. GpioInt)
 * @param interruptType variable to store interrupt type for specified GPIO pin.
 */
IOReturn VoodooGPIO::getInterruptType(int pin, int *interruptType) {
    SInt32 hw_pin = intel_gpio_to_pin(pin, nullptr, nullptr);
    if (hw_pin < 0)
        return kIOReturnNoInterrupt;

    // XXX: Need to Lie to macOS that the interrupt is Edge or it keeps disabling and re-enabling it (which we do ourselves)
    *interruptType = kIOInterruptTypeEdge;
    return kIOReturnSuccess;
}

/**
 * @param pin 'Software' pin number (i.e. GpioInt).
 */
IOReturn VoodooGPIO::registerInterrupt(int pin, OSObject *target, IOInterruptAction handler, void *refcon) {
    const struct intel_community *community;
    SInt32 hw_pin = intel_gpio_to_pin(pin, &community, nullptr);
    if (hw_pin < 0)
        return kIOReturnNoInterrupt;

    IOLog("%s::Registering hardware pin %d for GPIO IRQ pin %u", getName(), hw_pin, pin);

    unsigned communityidx = hw_pin - community->pin_base;
    
    if (community->pinInterruptActionOwners[communityidx])
        return kIOReturnNoResources;
    
    community->pinInterruptActionOwners[communityidx] = target;
    community->pinInterruptAction[communityidx] = handler;
    community->pinInterruptRefcons[communityidx] = refcon;
    return kIOReturnSuccess;
}

/**
 * @param pin 'Software' pin number (i.e. GpioInt).
 */
IOReturn VoodooGPIO::unregisterInterrupt(int pin) {
    const struct intel_community *community;
    SInt32 hw_pin = intel_gpio_to_pin(pin, &community, nullptr);
    if (hw_pin < 0)
        return kIOReturnNoInterrupt;

    intel_gpio_irq_mask_unmask(hw_pin, true);

    unsigned communityidx = hw_pin - community->pin_base;
    community->pinInterruptActionOwners[communityidx] = NULL;
    community->pinInterruptAction[communityidx] = NULL;
    community->interruptTypes[communityidx] = 0;
    community->pinInterruptRefcons[communityidx] = NULL;
    return kIOReturnSuccess;
}

/**
 * @param pin 'Software' pin number (i.e. GpioInt).
 */
IOReturn VoodooGPIO::enableInterrupt(int pin) {
    const struct intel_community *community;
    SInt32 hw_pin = intel_gpio_to_pin(pin, &community, nullptr);
    if (hw_pin < 0)
        return kIOReturnNoInterrupt;

    unsigned communityidx = hw_pin - community->pin_base;
    if (community->pinInterruptActionOwners[communityidx]) {
        intel_gpio_irq_set_type(hw_pin, community->interruptTypes[communityidx]);
        intel_gpio_irq_enable(hw_pin);
        return kIOReturnSuccess;
    }
    return kIOReturnNoInterrupt;
}

/**
 * @param pin 'Software' pin number (i.e. GpioInt).
 */
IOReturn VoodooGPIO::disableInterrupt(int pin) {
    SInt32 hw_pin = intel_gpio_to_pin(pin, nullptr, nullptr);
    if (hw_pin < 0)
        return kIOReturnNoInterrupt;

    intel_gpio_irq_mask_unmask(hw_pin, true);
    return kIOReturnSuccess;
}

/**
 * @param pin 'Software' pin number (i.e. GpioInt).
 * @param type Interrupt type to set for specified pin.
 */
IOReturn VoodooGPIO::setInterruptTypeForPin(int pin, int type) {
    const struct intel_community *community;
    SInt32 hw_pin = intel_gpio_to_pin(pin, &community, nullptr);
    if (hw_pin < 0)
        return kIOReturnNoInterrupt;

    unsigned communityidx = hw_pin - community->pin_base;
    community->interruptTypes[communityidx] = type;
    return kIOReturnSuccess;
}

void VoodooGPIO::InterruptOccurred(OSObject *owner, IOInterruptEventSource *src, int intCount) {
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooGPIO::interruptOccurredGated));
}
void VoodooGPIO::interruptOccurredGated() {
    for (int i = 0; i < ncommunities; i++) {
        struct intel_community *community = &communities[i];
        intel_gpio_community_irq_handler(community);
    }
}

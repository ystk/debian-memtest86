/* smp.c - MemTest-86 Version 4.1
 *
 * Routines for SMP operation.
 * Chris Brady, Bradytech Inc.
 */

#include "stdint.h"
#include "cpuid.h"
#include "smp.h"
#include "test.h"
#define DELAY_FACTOR 1

int num_cpus = 1; // There is at least one cpu, the BSP
int act_cpus;
unsigned found_cpus = 0;

extern void memcpy(void *dst, void *src , int len);
extern void test_start(void);
extern int run_cpus;
extern int maxcpus;
extern char cpu_mask[];
extern struct vars * const v;

struct barrier_s *barr;

void smp_find_cpus();

void barrier_init(int max)
{
	/* Set the adddress of the barrier structure */
	barr = (struct barrier_s *)(v->pmap[0].end << 12);
        barr->lck.slock = 1;
        barr->mutex.slock = 1;
        barr->maxproc = max;
        barr->count = max;
        barr->st1.slock = 1;
        barr->st2.slock = 0;
}

void s_barrier_init(int max)
{
        barr->s_lck.slock = 1;
        barr->s_maxproc = max;
        barr->s_count = max;
        barr->s_st1.slock = 1;
        barr->s_st2.slock = 0;
}

void barrier()
{
	if (num_cpus == 1) {
		return;
	}
	spin_wait(&barr->st1);     /* Wait if the barrier is active */
        spin_lock(&barr->lck);	   /* Get lock for barr struct */
        if (--barr->count == 0) {  /* Last process? */
                barr->st1.slock = 0;   /* Hold up any processes re-entering */
                barr->st2.slock = 1;   /* Release the other processes */
                barr->count++;
                spin_unlock(&barr->lck); 
        } else {
                spin_unlock(&barr->lck); 
                spin_wait(&barr->st2);	/* wait for peers to arrive */
                spin_lock(&barr->lck);   
                if (++barr->count == barr->maxproc) { 
                        barr->st1.slock = 1; 
                        barr->st2.slock = 0; 
                }
                spin_unlock(&barr->lck); 
        }
}

void s_barrier()
{
	if (run_cpus == 1) {
		return;
	}
	spin_wait(&barr->s_st1);     /* Wait if the barrier is active */
        spin_lock(&barr->s_lck);     /* Get lock for barr struct */
        if (--barr->s_count == 0) {  /* Last process? */
                barr->s_st1.slock = 0;   /* Hold up any processes re-entering */
                barr->s_st2.slock = 1;   /* Release the other processes */
                barr->s_count++;
                spin_unlock(&barr->s_lck); 
        } else {
                spin_unlock(&barr->s_lck); 
                spin_wait(&barr->s_st2);	/* wait for peers to arrive */
                spin_lock(&barr->s_lck);   
                if (++barr->s_count == barr->s_maxproc) { 
                        barr->s_st1.slock = 1; 
                        barr->s_st2.slock = 0; 
                }
                spin_unlock(&barr->s_lck); 
        }
}

typedef struct {
   bool started;
} ap_info_t;

volatile apic_register_t *APIC = NULL;
/* CPU number to APIC ID mapping table. CPU 0 is the BSP. */
static unsigned cpu_num_to_apic_id[MAX_CPUS];
volatile ap_info_t AP[MAX_CPUS];

void PUT_MEM16(uintptr_t addr, uint16_t val)
{
   *((volatile uint16_t *)addr) = val;
}

void PUT_MEM32(uintptr_t addr, uint32_t val)
{
   *((volatile uint32_t *)addr) = val;
}

static void inline 
APIC_WRITE(unsigned reg, uint32_t val)
{
   APIC[reg][0] = val;
}

static inline uint32_t 
APIC_READ(unsigned reg)
{
   return APIC[reg][0];
}


static void 
SEND_IPI(unsigned apic_id, unsigned trigger, unsigned level, unsigned mode,
	    uint8_t vector)
{
   uint32_t v;

   v = APIC_READ(APICR_ICRHI) & 0x00ffffff;
   APIC_WRITE(APICR_ICRHI, v | (apic_id << 24));

   v = APIC_READ(APICR_ICRLO) & ~0xcdfff;
   v |= (APIC_DEST_DEST << APIC_ICRLO_DEST_OFFSET) 
      | (trigger << APIC_ICRLO_TRIGGER_OFFSET)
      | (level << APIC_ICRLO_LEVEL_OFFSET)
      | (mode << APIC_ICRLO_DELMODE_OFFSET)
      | (vector);
   APIC_WRITE(APICR_ICRLO, v);
}


// Silly way of busywaiting, but we don't have a timer
void delay(unsigned us) 
{
   unsigned freq = 1000; // in MHz, assume 1GHz CPU speed
   uint64_t cycles = us * freq;
   uint64_t t0 = RDTSC();
   uint64_t t1;
   volatile unsigned k;

   do {
      for (k = 0; k < 1000; k++) continue;
      t1 = RDTSC();
   } while (t1 - t0 < cycles);
}

static inline void
memset (void *dst,
        char  value,
        int   len)
{
   int i;
   for (i = 0 ; i < len ; i++ ) { 
      *((char *) dst + i) = value;
   }
}

void kick_cpu(unsigned cpu_num)
{
   unsigned num_sipi, apic_id;
   apic_id = cpu_num_to_apic_id[cpu_num];

   // clear the APIC ESR register
   APIC_WRITE(APICR_ESR, 0);
   APIC_READ(APICR_ESR);

   // asserting the INIT IPI
   SEND_IPI(apic_id, APIC_TRIGGER_LEVEL, 1, APIC_DELMODE_INIT, 0);
   delay(100000 / DELAY_FACTOR);

   // de-assert the INIT IPI
   SEND_IPI(apic_id, APIC_TRIGGER_LEVEL, 0, APIC_DELMODE_INIT, 0);

   for (num_sipi = 0; num_sipi < 2; num_sipi++) {
      unsigned timeout;
      bool send_pending;
      unsigned err;

      APIC_WRITE(APICR_ESR, 0);

      SEND_IPI(apic_id, 0, 0, APIC_DELMODE_STARTUP, (unsigned)startup_32 >> 12);

      timeout = 0;
      do {
	 delay(10);
	 timeout++;
	 send_pending = (APIC_READ(APICR_ICRLO) & APIC_ICRLO_STATUS_MASK) != 0;
      } while (send_pending && timeout < 1000);

      if (send_pending) {
	 cprint(LINE_STATUS+1, 0, "SMP: STARTUP IPI was never sent");
      }
      
      delay(100000 / DELAY_FACTOR);

      err = APIC_READ(APICR_ESR) & 0xef;
      if (err) {
	 cprint(LINE_STATUS+1, 0, "SMP: After STARTUP IPI: err = 0x");
         hprint(LINE_STATUS+1, COL_MID, err);
      }
   }
}

// These memory locations are used for the trampoline code and data.

#define BOOTCODESTART 0x9000
#define GDTPOINTERADDR 0x9100
#define GDTADDR 0x9110

void boot_ap(unsigned cpu_num)
{
   unsigned num_sipi, apic_id;
   extern uint8_t gdt; 
   extern uint8_t _ap_trampoline_start;
   extern uint8_t _ap_trampoline_protmode;
   unsigned len = &_ap_trampoline_protmode - &_ap_trampoline_start;
   apic_id = cpu_num_to_apic_id[cpu_num];

   btrace(0, __LINE__, "Boot AP0  ", 1, cpu_num, 0);
   memcpy((uint8_t*)BOOTCODESTART, &_ap_trampoline_start, len);

   // Fixup the LGDT instruction to point to GDT pointer.
   PUT_MEM16(BOOTCODESTART + 3, GDTPOINTERADDR);

   // Copy a pointer to the temporary GDT to addr GDTPOINTERADDR.
   // The temporary gdt is at addr GDTADDR
   PUT_MEM16(GDTPOINTERADDR, 4 * 8);
   PUT_MEM32(GDTPOINTERADDR + 2, GDTADDR);

   // Copy the first 4 gdt entries from the currently used GDT to the
   // temporary GDT.
   memcpy((uint8_t *)GDTADDR, &gdt, 32);

   // clear the APIC ESR register
   APIC_WRITE(APICR_ESR, 0);
   APIC_READ(APICR_ESR);

   // asserting the INIT IPI
   SEND_IPI(apic_id, APIC_TRIGGER_LEVEL, 1, APIC_DELMODE_INIT, 0);
   delay(100000 / DELAY_FACTOR);

   // de-assert the INIT IPI
   SEND_IPI(apic_id, APIC_TRIGGER_LEVEL, 0, APIC_DELMODE_INIT, 0);
   btrace(0, __LINE__, "Boot AP1  ", 1, cpu_num, 0);

   for (num_sipi = 0; num_sipi < 2; num_sipi++) {
      unsigned timeout;
      bool send_pending;
      unsigned err;

      APIC_WRITE(APICR_ESR, 0);

      SEND_IPI(apic_id, 0, 0, APIC_DELMODE_STARTUP, BOOTCODESTART >> 12);

      timeout = 0;
      do {
	 delay(10);
	 timeout++;
	 send_pending = (APIC_READ(APICR_ICRLO) & APIC_ICRLO_STATUS_MASK) != 0;
      } while (send_pending && timeout < 1000);

      if (send_pending) {
	 cprint(LINE_STATUS+1, 0, "SMP: STARTUP IPI was never sent");
      }
      
      delay(100000 / DELAY_FACTOR);

      err = APIC_READ(APICR_ESR) & 0xef;
      if (err) {
	 cprint(LINE_STATUS+1, 0, "SMP: After STARTUP IPI: err = 0x");
         hprint(LINE_STATUS+1, COL_MID, err);
      }
   }
   btrace(0, __LINE__, "Boot AP2  ", 1, cpu_num, 0);
}

static int checksum(unsigned char *mp, int len)
{
   int sum = 0;

   while (len--) {
       sum += *mp++;
   }
   return (sum & 0xFF);
}

/* Parse an MP config table for CPU information */
bool read_mp_config_table(uintptr_t addr)
{
   mp_config_table_header_t *mpc = (mp_config_table_header_t*)addr;
   uint8_t *tab_entry_ptr;
   uint8_t *mpc_table_end;

   btrace(0, __LINE__, "read_mp 0 ", 1, (long)mpc, 0);
   if (mpc->signature != MPCSignature) {
      btrace(0, __LINE__, "read_mp 1 ", 1, (long)mpc->signature,
		(long)MPCSignature);
      return FALSE;
   }
   if (checksum((unsigned char*)mpc, mpc->length) != 0) {
      btrace(0, __LINE__, "read_mp 2 ", 1, (long)mpc->length, 0);
      return FALSE;
   }

   /* FIXME: the uintptr_t cast here works around a compilation problem on
    * AMD64, but it ignores the real problem, which is that lapic_addr
    * is only 32 bits.  Maybe that's OK, but it should be investigated.
    */
   APIC = (volatile apic_register_t*)(uintptr_t)mpc->lapic_addr;

   tab_entry_ptr = ((uint8_t*)mpc) + sizeof(mp_config_table_header_t);
   mpc_table_end = ((uint8_t*)mpc) + mpc->length;
   while (tab_entry_ptr < mpc_table_end) {
      switch (*tab_entry_ptr) {
      case MP_PROCESSOR: {
	 mp_processor_entry_t *pe = (mp_processor_entry_t*)tab_entry_ptr;

	 if (pe->cpu_flag & CPU_BOOTPROCESSOR) {
	    // BSP is CPU 0
	    cpu_num_to_apic_id[0] = pe->apic_id;
	 } else if (num_cpus < MAX_CPUS) {
	    cpu_num_to_apic_id[num_cpus] = pe->apic_id;
	    num_cpus++;
	 }
	 found_cpus++;
         btrace(0, __LINE__, "read_mp 3 ", 1, num_cpus, found_cpus);
	    
	 // we cannot handle non-local 82489DX apics
	 if ((pe->apic_ver & 0xf0) != 0x10) {
            btrace(0, __LINE__, "read_mp 4 ", 1, (long)pe->apic_ver, 0);
	    return 0;
	 }

	 tab_entry_ptr += sizeof(mp_processor_entry_t);
	 break;
      }
      case MP_BUS: {
	 tab_entry_ptr += sizeof(mp_bus_entry_t);
	 break;
      }
      case MP_IOAPIC: {
	 tab_entry_ptr += sizeof(mp_io_apic_entry_t);
	 break;
      }
      case MP_INTSRC:
	 tab_entry_ptr += sizeof(mp_interrupt_entry_t);
      case MP_LINTSRC:
	 tab_entry_ptr += sizeof(mp_local_interrupt_entry_t);
	 break;
      default: 
         btrace(0, __LINE__, "read_mp 5 ", 1, 0, 0);
	 return FALSE;
      }
   }
   return TRUE;
}

/* Search for a Floating Pointer structure */
floating_pointer_struct_t *
scan_for_floating_ptr_struct(unsigned int *addr, uint32_t length)
{
   floating_pointer_struct_t *fp;

   btrace(0, __LINE__, "scan_fps  ", 1, (long)addr, (long)length);
   while (length > 0) {
      fp = (floating_pointer_struct_t*)addr;
      if (*addr == FPSignature && fp->length == 1 && 
		checksum((unsigned char*)addr, 16) == 0 &&
		((fp->spec_rev == 1) || (fp->spec_rev == 4))) {
		
           btrace(0, __LINE__, "scan found", 1, (long)fp->spec_rev,
		*(long*)addr);
	   return fp;
      }
      addr += 4;
      length -= 16;
   }
   return NULL;
}

/* Search for a Root System Descriptor Pointer */
rsdp_t *scan_for_rsdp(uintptr_t addr, uint32_t length)
{
   rsdp_t *rp;
   uintptr_t end = addr + length;

   btrace(0, __LINE__, "scan_rsdp0", 1, (long)addr, (long)length);
   while ((uintptr_t)addr < end) {
      rp = (rsdp_t*)addr;
      if (*(unsigned int *)addr == RSDPSignature)
	  {
		  btrace(0, __LINE__, "found rsdp", 1, addr, rp->revision);
		  if (rp->revision == 0)
		  {
			  if (checksum((unsigned char*)addr, 20) == 0)
			  {
				  btrace(0, __LINE__, "scan_rsdp1", 1, *(long*)addr, RSDPSignature);
				  return rp;
			  }
		  }
		  else
		  {
			  if (checksum((unsigned char*)addr, rp->length) == 0)
			  {
				  btrace(0, __LINE__, "scan_rsdp1", 1, *(long*)addr, RSDPSignature);
				  return rp;
			  }
		  }
      }
      addr += 4;
   }
   return NULL;
}

/* Parse a MADT table for processor entries */
int parse_madt(uintptr_t addr) {

   mp_config_table_header_t *mpc = (mp_config_table_header_t*)addr;
   uint8_t *tab_entry_ptr;
   uint8_t *mpc_table_end;

   uint32_t logical_CPU_bits = 0; // number of bits in APIC ID used for hyperthreads

   btrace(0, __LINE__, "madt start", 1, (long)addr, 0);
   if (checksum((unsigned char*)mpc, mpc->length) != 0) {
      btrace(0, __LINE__, "madt csum ", 1, (long)mpc->length, 0);
      return FALSE;
   }
   
   if (!cpu_id.fid.bits.htt) // no hyperthreads
	   logical_CPU_bits = 0;
   else if (cpu_id.vend_id.char_array[0] == 'A') // AMD
   {
	   if (cpu_id.max_xcpuid >= 0x80000008)
	   {
		   unsigned int v[4];
		   uint32_t k = 0;
		   uint32_t threadCount = 0;

		   memset(v, 0, sizeof(v));
		   cpuid(0x80000008, &v[0], &v[1], &v[2], &v[3]);
		   if ((v[2] >> 12) & 0x0F)
		   {
			   uint32_t core_bits = (v[2] >> 12) & 0x0F;
			   threadCount = (cpu_id.info.bits.logicalProcessorCount >> core_bits);
		   }
		   else
		   {
			   uint32_t coreCount = (v[2] & 0xFF) + 1;
			   if (coreCount > 0)
				   threadCount = cpu_id.info.bits.logicalProcessorCount / coreCount;
		   }

		   btrace(0, __LINE__, "madt bits 8", 1, (long)threadCount, v[2]);
		   if (threadCount > 0)
		   {
			   k = threadCount * 2 - 1;	
			   logical_CPU_bits = 31;
			   while ( ((k & (1 << logical_CPU_bits)) == 0) && (logical_CPU_bits > 0))
			   {
				   logical_CPU_bits--;
			   }
		   }
	   }
	   btrace(0, __LINE__, "madt bits a", 1, (long)logical_CPU_bits, 0);
   }
   else if (cpu_id.vend_id.char_array[0] == 'G') // Intel
   {
	   unsigned int v[4];
	   int levelType, levelShift;

	   memset(v, 0, sizeof(v));
	   if (cpu_id.max_cpuid >= 0xb)
		   cpuid_count(0xb, 0, &v[0], &v[1], &v[2], &v[3]);

	   if (v[1] != 0) // ebx != 0
	   {
		   levelType = (v[2] >> 8) & 0xFF;
		   levelShift = v[0] & 0xF;

		   if (levelType == 1)
			   logical_CPU_bits = levelShift;		   

		   btrace(0, __LINE__, "madt bits b", 1, (long)logical_CPU_bits, (long)levelShift);
	   }
	   else 
	   {
		   uint32_t threadCount = 0;
		   uint32_t k = 0;

		   if (cpu_id.max_cpuid >= 4)
		   {
			   uint32_t coreCount = 0;
		   
			   cpuid_count(4, 0, &v[0], &v[1], &v[2], &v[3]);

			   coreCount = ((v[0] >> 26) & 0x3F) + 1;

			   if (coreCount > 0)
				   threadCount = cpu_id.info.bits.logicalProcessorCount / coreCount;

			   btrace(0, __LINE__, "madt bits 4", 1, (long)threadCount, (long)coreCount);
		   }
		   else
		   {
			   threadCount = cpu_id.info.bits.logicalProcessorCount;			   
			   btrace(0, __LINE__, "madt bits 1", 1, (long)threadCount, 0);
		   }

		   if (threadCount > 0)
		   {
			   k = threadCount * 2 - 1;	
			   logical_CPU_bits = 31;
			   while ( ((k & (1 << logical_CPU_bits)) == 0) && (logical_CPU_bits > 0))
			   {
				   logical_CPU_bits--;
			   }
		   }
		   btrace(0, __LINE__, "madt bits i", 1, (long)logical_CPU_bits, 0);
	   }	   
   }

   APIC = (volatile apic_register_t*)(uintptr_t)mpc->lapic_addr;

   tab_entry_ptr = ((uint8_t*)mpc) + sizeof(mp_config_table_header_t);
   mpc_table_end = ((uint8_t*)mpc) + mpc->length;
   while (tab_entry_ptr < mpc_table_end) {

      madt_processor_entry_t *pe = (madt_processor_entry_t*)tab_entry_ptr;
      if (pe->type == MP_PROCESSOR) {
	 if (pe->enabled) {
		 bool duplicate_cpu = FALSE;
	     if (num_cpus < MAX_CPUS) {
			 if (found_cpus == 0)
				 cpu_num_to_apic_id[0] = pe->apic_id;
			 else
			 {
				 bool found_thread = FALSE;
				 int i;
				 for (i = 0; i < num_cpus; i++)
				 {
					 if ( (cpu_num_to_apic_id[i] & ~( (1 << logical_CPU_bits) -1)) == (pe->apic_id & ~( (1 << logical_CPU_bits) -1)) )
					 {
						 found_thread = TRUE;
						 btrace(0, __LINE__, "madt thread", 1, (long)pe->apic_id, (long)cpu_num_to_apic_id[i]);

						 if ((long)pe->apic_id == (long)cpu_num_to_apic_id[i])
							 duplicate_cpu = TRUE;

						 break;
					 }
				 }

				 if (!found_thread)
				 {
					cpu_num_to_apic_id[num_cpus] = pe->apic_id;

					/* the first CPU is the BSP, don't increment */
					if (found_cpus) {
						num_cpus++;
      						btrace(0, __LINE__, "madt found", 1, (long)num_cpus,
						(long)found_cpus);
					}
				 }
			 }
	     }
		 if (!duplicate_cpu)
			found_cpus++;
	 }
      }
       tab_entry_ptr += pe->length;
   }
   return TRUE;
}

void initialise_cpus(void)
{
	int i;

	act_cpus = 0;
	btrace(0, __LINE__, "init_cpus0", 1, maxcpus, 0);
	if (maxcpus > 1) {
		smp_find_cpus();
		/* The total number of CPUs may be limited */
		if (num_cpus > maxcpus) {
			num_cpus = maxcpus;
		}
		/* Determine how many cpus have been selected */
		for(i = 0; i < num_cpus; i++) {
			if (cpu_mask[i]) {
				act_cpus++;
			}
		}
	} else {
		act_cpus = found_cpus = num_cpus = 1;
	}

	/* Initialize the barrier before starting AP's */
	barrier_init(act_cpus);

	/* let the BSP initialise the APs. */
	for(i = 1; i < num_cpus; i++) {
	    /* Only start this CPU if it is selected by the mask */
	    if (cpu_mask[i]) {
	        smp_boot_ap(i);
	    }
	}
	btrace(0, __LINE__, "init_cpus1", 1, num_cpus, act_cpus);
}

/* This is where we search for SMP information in the following order
 * look for a floating MP pointer
 *   found:
 *     check for a default configuration
 * 	 found:
 *	   setup config, return
 *     check for a MP config table
 *	 found:
 *	   validate:
 *           good:
 *	        parse the MP config table
 *		  good:
 *		    setup config, return
 *
 * find & validate ACPI RSDP (Root System Descriptor Pointer)
 *   found:
 *     find & validate RSDT (Root System Descriptor Table)
 *       found:
 *         find & validate MSDT
 *	     found:
 *             parse the MADT table
 *               good:
 *		   setup config, return
 */
void smp_find_cpus()
{
   floating_pointer_struct_t *fp;
   rsdp_t *rp;
   rsdt_t *rt;
   uint8_t *tab_ptr, *tab_end;
   unsigned int *ptr;

   btrace(0, __LINE__, "smp_find_0", 1, 0, 0);

   memset(&AP, 0, sizeof AP);

   /* No MP table so far, try to find an ACPI MADT table
    * We try to use the MP table first since there is no way to distinguish
    * real cores from hyper-threads in the MADT */

   /* Search for the RSDP */
   rp = scan_for_rsdp(0xe0000, 0x20000);
   btrace(0, __LINE__, "smp_find_4", 1, (long)rp, 0);
   if (rp == NULL) {
        /* Search the BIOS ESDS area */
        unsigned int address = *(unsigned short *)0x40E;
        address <<= 4;
	if (address) {
       		rp = scan_for_rsdp(address, 0x400);
   		btrace(0, __LINE__, "smp_find_5", 1, (long)rp, 0);
        }
   }

   do
   {
	   if (rp == NULL) {
		/* RSDP not found, give up */
		break;
	   }

	   /* Found the RSDP, now get either the RSDP or XRSDP */
	   if (rp->revision >= 2) {
			rt = (rsdt_t *)rp->xrsdt[0];
			if (rt == 0) {
   				btrace(0, __LINE__, "smp_find_7", 1, (long)rt, 0);
				break;
			}
			/* Validate the XSDT */
			if (*(unsigned int *)rt != XSDTSignature) {
   				btrace(0, __LINE__, "smp_find_8", 1, *(long*)rt,
					XSDTSignature);
				break;
			}
			if ( checksum((unsigned char*)rt, rt->length) != 0) {
   				btrace(0, __LINE__, "smp_find_9", 1,
					(long)rt->length, 0);
				break;
			}
		} else {
			rt = (rsdt_t *)rp->rsdt;
			if (rt == 0) {
   				btrace(0, __LINE__, "smp_find10", 1, 0, 0);
				break;
			}
			/* Validate the RSDT */
			if (*(unsigned int *)rt != RSDTSignature) {
   				btrace(0, __LINE__, "smp_find11", 1,
						*(long*)rt, RSDTSignature);
				break;
			}
			if ( checksum((unsigned char*)rt, rt->length) != 0) {
   				btrace(0, __LINE__,"smp_find12",1,(long)rt->length,0);
				break;
			}
		}

		/* Scan the RSDT or XSDT for a pointer to the MADT */
		tab_ptr = ((uint8_t*)rt) + sizeof(rsdt_t);
		tab_end = ((uint8_t*)rt) + rt->length;

		while (tab_ptr < tab_end) {
		ptr = *(unsigned int *)tab_ptr;
		/* Check for the MADT signature */
		if (ptr && *ptr == MADTSignature) {

			/* Found it, now parse it */
			if (parse_madt((uintptr_t)ptr)) {
   			btrace(0, __LINE__,"smp_find13", 1, (long)ptr, 0);
			return;
			}
		}
			tab_ptr += 4;
		}
   } while (FALSE);
   
   /* Search for the Floating MP structure pointer */
   fp = scan_for_floating_ptr_struct(0x0, 0x400);
   if (fp == NULL) {
      fp = scan_for_floating_ptr_struct(639*0x400, 0x400);
   }
   if (fp == NULL) {
         fp = scan_for_floating_ptr_struct(0xf0000, 0x10000);
   }
   if (fp == NULL) {
        /* Search the BIOS ESDS area */
        unsigned int address = *(unsigned short *)0x40E;
        address <<= 4;
	if (address) {
       		fp = scan_for_floating_ptr_struct(address, 0x400);
        }
   }
   btrace(0, __LINE__, "smp_find_1", 1, (long)fp, 0);

   if (fp != NULL) {
		/* We have a floating MP pointer */

		/* Is this a default configuration? */
		if (fp->feature[0] > 0 && fp->feature[0] <=7) {
			/* This is a default config so plug in the numbers */
			num_cpus = 2;
			APIC = 0xfee00000;
			cpu_num_to_apic_id[0] = 0;
			cpu_num_to_apic_id[1] = 1;
   			btrace(0, __LINE__, "smp_find_2", 1, 0, 0);
			return;
		}

		/* Do we have a pointer to a MP configuration table? */
		if ( fp->phys_addr != 0) {
			if (read_mp_config_table(fp->phys_addr)) {
				/* Found a good MP table, done */
				btrace(0, __LINE__, "smp_find_3", 1, (long)fp->phys_addr, 0);

				/* Only return here if we found more than one CPU. The MADT
				* may adentify more than one. */
				if (num_cpus > 1) {
					return;
				}
			}
		}
    }
}
	
unsigned my_apic_id()
{
   return (APIC[APICR_ID][0]) >> 24;
}

void smp_ap_booted(unsigned cpu_num) 
{
   AP[cpu_num].started = TRUE;
}

void smp_boot_ap(unsigned cpu_num)
{
   unsigned timeout;
   boot_ap(cpu_num);
   timeout = 0;
   do {
      delay(1000 / DELAY_FACTOR);
      timeout++;
   } while (!AP[cpu_num].started && timeout < 100000 / DELAY_FACTOR);

   if (!AP[cpu_num].started) {
      cprint(LINE_STATUS+1, 0, "SMP: Boot timeout for");
      dprint(LINE_STATUS+1, COL_MID, cpu_num,2,1);
      cprint(LINE_STATUS+1, 26, "Turning off SMP");
   }
}

unsigned smp_my_cpu_num()
{
   unsigned apicid = my_apic_id();
   unsigned i;

   for (i = 0; i < MAX_CPUS; i++) {
      if (apicid == cpu_num_to_apic_id[i]) {
	 break;
      }
   }
   if (i == MAX_CPUS) {
      i = 0;
   }
   return i;
}

/* A set of simple functions used to preserve assigned CPU ordinals since
 * they are lost after relocation (the stack is reloaded).
 */
int num_to_ord[MAX_CPUS];
void smp_set_ordinal(int me, int ord)
{
	num_to_ord[me] = ord;
}

int smp_my_ord_num(int me)
{
	return num_to_ord[me];
}

int smp_ord_to_cpu(int me)
{
	int i;
	for (i=0; i<MAX_CPUS; i++) {
		if (num_to_ord[i] == me) return i;
	}
	return -1;
}


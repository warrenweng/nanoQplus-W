/************************************************************************/
/*                                                                      */
/*  PROJECT : Flash Memory Management S/W for ETRI Nano OS              */
/*  MODULE  : CFD (Common Flash Driver)                                 */
/*  FILE    : fsmc_nand_lld.c                                           */
/*  PURPOSE : Low-Level Device Driver for NAND flash via STM32F4 FSMC   */
/*                                                                      */
/*----------------------------------------------------------------------*/
/*  Authors : Zeen Information Technologies, Inc.                       */
/*            (info@zeen.snu.ac.kr)                                     */
/*----------------------------------------------------------------------*/
/*  The copyright of this software is subject to the copyright of ETRI  */
/*  Nano OS. For more information, please contact authorities of ETRI.  */
/*----------------------------------------------------------------------*/
/*  NOTES                                                               */
/*                                                                      */
/*----------------------------------------------------------------------*/
/*  REVISION HISTORY (Ver 0.9)                                          */
/*                                                                      */
/*  - 2016.01.15 [Joosun Hahn]   : First writing                        */
/*  - 2016.02.17 [Sung-Kwan Kim] : Revised to adapt to CFD framework    */
/*                                                                      */
/************************************************************************/

#include "kconf.h"
#ifdef CFD_M

#include "fd_config.h"
#include "fd_if.h"
#include "fd_physical.h"
#include "ecc_512b.h"

#include "stm32f4xx_fsmc.h"
#include "fsmc_nand.h"
#include "fsmc_nand_lld.h"

#include <string.h>
#include <stdio.h>

/*----------------------------------------------------------------------*/
/*  Constant & Macro Definitions                                        */
/*----------------------------------------------------------------------*/

#define ASYNC_MODE          1       /* asynchronous mode setting;
                                       if this is set to 1, after sending
                                       a flash command to the flash controller,
                                       the driver does not wait for the result;
                                       instead, it returns immediately and 
                                       check the result before processing the
                                       next flash command (default is 1) */

#define NAND_BANK                   FSMC_Bank2_NAND
#define NAND_BASE_ADDR              ((uint32_t)0x70000000)
#define NAND_ATTR_ADDR              ((uint32_t)0x78000000)
#define ROW_ADDRESS(block, page)    ((page) + ((block) * NAND_BLOCK_SIZE))

#define WAIT_FLASH_READY()                                          \
        do {                                                        \
            while (GPIO_ReadInputDataBit(GPIOD, GPIO_Pin_6) == 0);  \
        } while (0)

#define WAIT_DELAY(delay)                                           \
        do {                                                        \
            int x;                                                  \
            for (x = 0; x < delay; x++);                            \
        } while (0)

#define DMA_CLK                     RCC_AHB1Periph_DMA2
#define DMA_DATA_SIZE_MASK          0xFFFF87FF

/* DMA (READ: NAND --> Memory) related macros */
#define DMA_RD_CHANNEL              DMA_Channel_3
#define DMA_RD_STREAM               DMA2_Stream1
#define DMA_RD_TCIF                 DMA_FLAG_TCIF1
#define DMA_RD_IRQN                 DMA2_Stream1_IRQn

/* DMA (WRITE: Memory --> NAND) related macros */
#define DMA_WR_CHANNEL              DMA_Channel_1
#define DMA_WR_STREAM               DMA2_Stream0
#define DMA_WR_TCIF                 DMA_FLAG_TCIF0
#define DMA_WR_IRQN                 DMA2_Stream0_IRQn

/* macros for data transfer methods */

#if 1
#define DMA_OK(buf)                 ((uint32_t)(buf) >= 0x20000000)
#else
#define DMA_OK(buf)                 0
#endif

#define DATA_READ(buf, size)                                \
        do {                                                \
            if (DMA_OK(buf)) data_read_dma(buf, size);      \
            else data_read(buf, size);                      \
        } while (0)

#define DATA_WRITE(buf, size)                               \
        do {                                                \
            if (DMA_OK(buf)) data_write_dma(buf, size);     \
            else data_write(buf, size);                     \
        } while (0)

/*----------------------------------------------------------------------*/
/*  Type Definitions                                                    */
/*----------------------------------------------------------------------*/

/* NAND information encoded in NAND ID */

typedef struct {
    uint32_t        internal_chip_number;
    uint32_t        cell_type;
    uint32_t        simul_prog_pages;
    uint32_t        interleave_support;
    uint32_t        cache_prog_support;
    uint32_t        page_size;
    uint32_t        block_size;
    uint32_t        spare_size_per_512;
    uint32_t        organization;
    uint32_t        serial_access_min;
    uint32_t        plane_number;
    uint32_t        plane_size;
} nand_info_t;

/*----------------------------------------------------------------------*/
/*  External Variable Definitions                                       */
/*----------------------------------------------------------------------*/

/*----------------------------------------------------------------------*/
/*  Local Variable Definitions                                          */
/*----------------------------------------------------------------------*/

static nand_info_t          nand_info[NAND_NUM_CHIPS];

static flash_chip_spec_t    flash_spec[NAND_NUM_CHIPS] = {
    {   /* for chip-0         */
        /* type               */    SLC_NAND_FLASH,
        /* page_size          */    (2048 + 64),
        /* data_size          */    2048,
        /* spare_size         */    64,
        /* sectors_per_page   */    4,
        /* pages_per_block    */    64,
        /* block_size         */    (2048 * 64),
        /* num_blocks         */    1024,
        /* num_dies_per_ce    */    1,
        /* num_planes         */    1,
        /* max_num_bad_blocks */    25,
        /* constraints        */    0
    }
};

/*----------------------------------------------------------------------*/
/*  Local Function Declarations                                         */
/*----------------------------------------------------------------------*/

static uint32_t fsmc_nand_reset(uint16_t chip);
static uint32_t fsmc_nand_get_status(uint16_t chip);

static void     data_read(uint8_t *buf, uint32_t size);
static void     data_read_dma(uint8_t *buf, uint32_t size);
static void     data_write(uint8_t *buf, uint32_t size);
static void     data_write_dma(uint8_t *buf, uint32_t size);

#if (ECC_METHOD == HW_ECC)
static int32_t  ecc_correct_data(uint32_t ecc_calc, uint32_t ecc_read, uint8_t *data);
#endif


/*======================================================================*/
/*  Function Definitions                                                */
/*======================================================================*/

/*----------------------------------------------------------------------*/
/*  Global Function Definitions                                         */
/*----------------------------------------------------------------------*/

int32_t fsmc_nand_init(void)
{
    int32_t           err;
    uint16_t          i;
    flash_chip_ops_t  flash_ops;
    nand_id_t         nand_id;

    GPIO_InitTypeDef                   GPIO_InitStructure; 
    FSMC_NANDInitTypeDef               FSMC_NANDInitStructure;
    FSMC_NAND_PCCARDTimingInitTypeDef  p1, p2;
    NVIC_InitTypeDef                   NVIC_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD | RCC_AHB1Periph_GPIOE, ENABLE);

    /*-- GPIO Configuration --------------------------------------*/
    /* CLE, ALE, D0->D7, NOE, NWE and NCE2 NAND pin configuration */

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    RCC_AHB3PeriphClockCmd(RCC_AHB3Periph_FSMC, ENABLE);
	
    /* D0->D3 */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource14, GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource15, GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource0,  GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource1,  GPIO_AF_FSMC);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_0 | GPIO_Pin_1 |GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    /* D4->D7 NAND pin configuration */
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource7,  GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource8,  GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource9,  GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOE, GPIO_PinSource10, GPIO_AF_FSMC);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_Init(GPIOE, &GPIO_InitStructure);

    /* NOE, NWE */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource11, GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource12, GPIO_AF_FSMC);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    /* NCE and NCE2 */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource4, GPIO_AF_FSMC);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource5, GPIO_AF_FSMC);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_5;
    GPIO_Init(GPIOD, &GPIO_InitStructure);
    
    /* RB (Ready/nBusy) */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource6, GPIO_AF_FSMC);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_DOWN;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    /* CS->PD13 */
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource13, GPIO_AF_FSMC);
    
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_13;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    /* FSMC setup */
    FSMC_NANDInitStructure.FSMC_Bank            = NAND_BANK;
    FSMC_NANDInitStructure.FSMC_Waitfeature     = FSMC_Waitfeature_Disable;
    FSMC_NANDInitStructure.FSMC_MemoryDataWidth = FSMC_MemoryDataWidth_8b;
    FSMC_NANDInitStructure.FSMC_ECC             = FSMC_ECC_Disable;
    FSMC_NANDInitStructure.FSMC_ECCPageSize     = FSMC_ECCPageSize_2048Bytes;
    FSMC_NANDInitStructure.FSMC_TCLRSetupTime   = 0;
    FSMC_NANDInitStructure.FSMC_TARSetupTime    = 0;
    FSMC_NANDInitStructure.FSMC_CommonSpaceTimingStruct    = &p1;
    FSMC_NANDInitStructure.FSMC_AttributeSpaceTimingStruct = &p2;

    p1.FSMC_SetupTime     = 0;   // minimum 1
    p1.FSMC_WaitSetupTime = 1;   // minimum 2
    p1.FSMC_HoldSetupTime = 0;   // minimum 1
    p1.FSMC_HiZSetupTime  = 0;   // minimum 0
    
    p2.FSMC_SetupTime     = 1;   // minimum 1
    p2.FSMC_WaitSetupTime = 4;   // minimum 2
    p2.FSMC_HoldSetupTime = 1;   // minimum 1
    p2.FSMC_HiZSetupTime  = 0;   // minimum 0

    FSMC_NANDDeInit(NAND_BANK);
    FSMC_NANDInit(&FSMC_NANDInitStructure);
    FSMC_NANDCmd(NAND_BANK, ENABLE);

    /* interrupt setup for DMA (disable) */
    RCC_AHB1PeriphClockCmd(DMA_CLK, ENABLE);
    DMA_DeInit(DMA_RD_STREAM);
    DMA_DeInit(DMA_WR_STREAM);
    
    NVIC_InitStructure.NVIC_IRQChannel                   = DMA_RD_IRQN;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = DISABLE;
    NVIC_Init(&NVIC_InitStructure);
    
    NVIC_InitStructure.NVIC_IRQChannel                   = DMA_WR_IRQN;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = DISABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* register flash operations */
    MEMSET((void *)&flash_ops, 0, sizeof(flash_chip_ops_t));
    flash_ops.open          = fsmc_nand_open;
    flash_ops.close         = fsmc_nand_close;
    flash_ops.read_page     = fsmc_nand_read_page;
    flash_ops.read_bytes    = fsmc_nand_read_bytes;
    flash_ops.write_page    = fsmc_nand_write_page;
    flash_ops.write_bytes   = fsmc_nand_write_bytes;
    flash_ops.erase         = fsmc_nand_erase;
    flash_ops.is_bad_block  = fsmc_nand_is_bad_block;
#if (ASYNC_MODE == 1)
    flash_ops.sync          = fsmc_nand_sync;
#else
    flash_ops.sync          = NULL;
#endif

    /* register flash memory chips to CFD (Common Flash Driver) */
    for (i = 0; i < NAND_NUM_CHIPS; i++) {

        /* reset NAND flash chip */
        fsmc_nand_reset(i);
        WAIT_FLASH_READY();
    
        /* read NAND flash id */
        fsmc_nand_read_id(i, &nand_id);

        /* register each physical flash device (chip) */
        err = pfd_register_flash_chip(i,
                                      &flash_spec[i],
                                      &flash_ops);
        if (err) return(FM_INIT_FAIL);
    }
    
    return(FM_SUCCESS);
}


int32_t fsmc_nand_open(uint16_t chip)
{
    return(FM_SUCCESS);
}


int32_t fsmc_nand_close(uint16_t chip)
{
    return(FM_SUCCESS);
}


int32_t fsmc_nand_read_page(uint16_t chip, uint32_t block, uint16_t page,
                            uint8_t *dbuf, uint8_t *sbuf)
{
    int32_t err = FM_SUCCESS;

#if (ECC_METHOD == HW_ECC)
    int32_t  result, retry = 0;
    uint32_t ecc_calc, ecc_read;
    uint8_t  __sbuf[16 * MAX_SECTORS_PER_PAGE];
    
    if (sbuf == NULL) sbuf = __sbuf;
#endif
    
    if (dbuf == NULL && sbuf == NULL) return(FM_ERROR);

    /* send the page-read command and page address */
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_READ_1; 
   
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00; 
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = (dbuf != NULL ? 0x00 : 0x08); 
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_1st_CYCLE(ROW_ADDRESS(block, page));  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_2nd_CYCLE(ROW_ADDRESS(block, page));  
    
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_READ_TRUE; 

    WAIT_FLASH_READY();
    WAIT_DELAY(5);

    /* read data into the given buffers */    
    if (dbuf != NULL) {

#if (ECC_METHOD == HW_ECC)
        /* enable ECC */
        FSMC_NANDECCCmd(NAND_BANK, ENABLE);
#endif

        /* read the main area data */
        DATA_READ(dbuf, flash_spec[chip].data_size);

#if (ECC_METHOD == HW_ECC)
        /* read ECC parity bytes from the ECC register */
        ecc_calc = FSMC_GetECC(NAND_BANK);
        ecc_calc ^= 0xFFFFFFFF;

        /* disable ECC */
        FSMC_NANDECCCmd(NAND_BANK, DISABLE);
#endif

        if (sbuf != NULL) {
            /* send the random output command to read data in the spare area */
            *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_RANDOMOUT; 
           
            *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00; 
            *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x08; 
            
            *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_RANDOMOUT_TRUE;
            
            WAIT_DELAY(5);
        
            /* read the spare area data */   
            DATA_READ(sbuf, flash_spec[chip].spare_size);
        }
    }
    else {
        /* read the spare area data only */
        DATA_READ(sbuf, flash_spec[chip].spare_size);
    }

#if (ECC_METHOD == HW_ECC)
    if (dbuf != NULL) {
        ecc_read = *(uint32_t *)(sbuf + 8);

ecc_retry:
        if (ecc_calc != ecc_read) {
            
            /* ECC mismatch; try to correct errors */
            result = ecc_correct_data(ecc_calc, ecc_read, dbuf);
            switch (result) {
            case ECC_NO_ERROR:
                break;
            
            case ECC_CORRECTABLE_ERROR:
                printf("[LLD] ECC correction OK (block = %d, page = %d)\r\n",
                       (int)block, (int)page);
                break;
    
            case ECC_ECC_ERROR:
                if (retry == 0) {
                    retry = 1;
                    ecc_read = *(uint32_t *)(sbuf + 12);
                    printf("[LLD] Trying again using an ECC copy ...\r\n");
                    goto ecc_retry;
                }
                /* retry failed; fall through below */
                
            default:
                printf("[LLD] UNCORRECTABLE ECC ERROR (block = %d, page = %d) !!!\r\n",
                       (int)block, (int)page);
                err = FM_ECC_ERROR;
                break;
            }
        }
    }
#endif

    return(err);
}


int32_t fsmc_nand_read_bytes(uint16_t chip, uint32_t block, uint16_t page,
                             uint32_t num_bytes, uint8_t *dbuf)
{
    if (dbuf == NULL) return(FM_ERROR);
    if (num_bytes < 4) return(FM_ERROR);

    /* send the page-read command and page address */
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_READ_1; 
   
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00; 
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00; 
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_1st_CYCLE(ROW_ADDRESS(block, page));  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_2nd_CYCLE(ROW_ADDRESS(block, page));  
    
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_READ_TRUE; 
    
    WAIT_FLASH_READY();
    WAIT_DELAY(2);

    /* read data into the given buffers */
    DATA_READ(dbuf, num_bytes);

    return(FM_SUCCESS);
}


int32_t fsmc_nand_write_page(uint16_t chip, uint32_t block, uint16_t page,
                             uint8_t *dbuf, uint8_t *sbuf, bool_t is_last)
{
    uint32_t status = NAND_READY;

#if (ECC_METHOD == HW_ECC)
    uint32_t i, ecc_calc;
    uint8_t  __sbuf[16 * MAX_SECTORS_PER_PAGE];
    
    if (sbuf == NULL) {
        sbuf = __sbuf;
        for (i = 0; i < 16 * MAX_SECTORS_PER_PAGE; i++) sbuf[i] = 0xFF;
    }
#endif
    
    if (dbuf == NULL && sbuf == NULL) return(FM_ERROR);

    /* send the page-program command and page address */
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_PAGEPROGRAM;

    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00;  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = (dbuf != NULL ? 0x00 : 0x08);  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_1st_CYCLE(ROW_ADDRESS(block, page));
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_2nd_CYCLE(ROW_ADDRESS(block, page));
    
    //WAIT_DELAY(9);

    /* send data in the given buffer to flash memory */
    if (dbuf != NULL) {

#if (ECC_METHOD == HW_ECC)
        /* enable ECC */
        FSMC_NANDECCCmd(NAND_BANK, ENABLE);
#endif

        /* write the spare area data */
        DATA_WRITE(dbuf, flash_spec[chip].data_size);

#if (ECC_METHOD == HW_ECC)
        /* read ECC parity bytes from the ECC register and store them */
        ecc_calc = FSMC_GetECC(NAND_BANK);
        ecc_calc ^= 0xFFFFFFFF;

        /* disable ECC */
        FSMC_NANDECCCmd(NAND_BANK, DISABLE);
        
        /* store ECC and an ECC copy */
        *(uint32_t *)(sbuf +  8) = ecc_calc;
        *(uint32_t *)(sbuf + 12) = ecc_calc;
#endif
    
        if (sbuf != NULL) {
            /* send the random input command to write data in the spare area */
            *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_RANDOMIN; 
           
            *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00; 
            *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x08;
            
            //WAIT_DELAY(9);
        
            /* write the spare area data */
            DATA_WRITE(sbuf, flash_spec[chip].spare_size);
        }
    }
    else {
        /* write the spare area data only */
        DATA_WRITE(sbuf, flash_spec[chip].spare_size);
    }
    
    //WAIT_DELAY(3);

    /* send the program confirm command */
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_PAGEPROGRAM_TRUE;

#if (ASYNC_MODE == 0)
    status = fsmc_nand_get_status(chip);
#endif

    if (status == NAND_READY) return(FM_SUCCESS);
    else return(FM_WRITE_ERROR);
}


int32_t fsmc_nand_write_bytes(uint16_t chip, uint32_t block, uint16_t page,
                              uint32_t num_bytes, uint8_t *dbuf)
{
    uint32_t status = NAND_READY;
    
    if (dbuf == NULL) return(FM_ERROR);
    if (num_bytes < 4) return(FM_ERROR);

    /* send the page-program command and page address */
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_PAGEPROGRAM;
   
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00;  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00;  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_1st_CYCLE(ROW_ADDRESS(block, page));
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_2nd_CYCLE(ROW_ADDRESS(block, page));
    
    //WAIT_DELAY(9);
    
    /* write data from the given buffers */
    DATA_WRITE(dbuf, num_bytes);
    
    //WAIT_DELAY(3);

    /* send the program confirm command */
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_PAGEPROGRAM_TRUE;

#if (ASYNC_MODE == 0)
    status = fsmc_nand_get_status(chip);
#endif

    if (status == NAND_READY) return(FM_SUCCESS);
    else return(FM_WRITE_ERROR);
}


int32_t fsmc_nand_erase(uint16_t chip, uint32_t block)
{
    uint32_t status = NAND_READY;
    
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_ERASE0;
  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_1st_CYCLE(ROW_ADDRESS(block, 0));  
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = ADDR_2nd_CYCLE(ROW_ADDRESS(block, 0));

    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_ERASE1;

#if (ASYNC_MODE == 0)
    status = fsmc_nand_get_status(chip);
#endif

    if (status == NAND_READY) return(FM_SUCCESS);
    else return(FM_ERASE_ERROR);
}


int32_t fsmc_nand_sync(uint16_t chip, uint16_t prev_command)
{
    uint32_t status;
    
    status = fsmc_nand_get_status(chip);
    
    if (status == NAND_READY) return(FM_SUCCESS);
    else return(FM_ERROR);
}


bool_t fsmc_nand_is_bad_block(uint16_t chip, uint32_t block)
{
    uint8_t  buf[NAND_SPARE_AREA_SIZE];
    uint16_t check_page[2];
    
    // check the first byte in spare area of the 1st and 2nd page;
    // if it is not 0xFF, the corresponding block is a bad block;
    check_page[0] = 0;
    check_page[1] = 1;
    
    if (fsmc_nand_read_page(chip, block, check_page[0], NULL, buf) != 0)
        return(TRUE);

    if (buf[0] != 0xFF) return(TRUE);

    if (fsmc_nand_read_page(chip, block, check_page[1], NULL, buf) != 0)
        return(1);

    if (buf[0] != 0xFF) return(TRUE);
    
    return(FALSE);
}


int32_t fsmc_nand_read_id(uint16_t chip, nand_id_t *nand_id)
{
    nand_info_t *n = &nand_info[chip];

    /* send the read-id command */     
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA)  = NAND_CMD_READID;
    *(vu8 *)(NAND_BASE_ADDR | ADDR_AREA) = 0x00;

    /* sequence to read ID from NAND flash */
    nand_id->maker_id  = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);
    nand_id->device_id = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);
    nand_id->id3       = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);
    nand_id->id4       = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);
    nand_id->id5       = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);

    printf("--------------------------------------------------\r\n");
    printf("Nand Flash ID = %02X,%02X,%02X,%02X,%02X  ", 
           nand_id->maker_id, nand_id->device_id, nand_id->id3, nand_id->id4, nand_id->id5);
         
    if ((nand_id->maker_id == 0xEC) && (nand_id->device_id == 0xF1) &&
        (nand_id->id3 == 0x80) && (nand_id->id4 == 0x15)) {
        printf("Type = K9F1G08U0A\r\n");
    }
    else if ((nand_id->maker_id == 0xEC) && (nand_id->device_id == 0xF1) &&
             (nand_id->id3 == 0x00) && (nand_id->id4 == 0x95)) {
        printf("Type = K9F1G08U0B / K9F1G08U0C\r\n");        
    }
    else if ((nand_id->maker_id == 0xEC) && (nand_id->device_id == 0xF1) &&
             (nand_id->id3 == 0x00) && (nand_id->id4 == 0x15)) {
        printf("Type = K9F1G08U0D\r\n");        
    }
    else if ((nand_id->maker_id == 0xAD) && (nand_id->device_id == 0xF1) &&
             (nand_id->id3 == 0x80) && (nand_id->id4 == 0x1D)) {
        printf("Type = HY27UF081G2A\r\n");        
    }
    else {
        printf("Type = Unknown\r\n");
    }
    printf("--------------------------------------------------\r\n");

    /* extract information from the 3rd ID */
    n->internal_chip_number = 1 << (nand_id->id3 & 0x03);
    n->cell_type            = 2 << ((nand_id->id3 & 0x0c) >> 2);
    n->simul_prog_pages     = 1 << ((nand_id->id3 & 0x30) >> 4);
    n->interleave_support   = (nand_id->id3 & 0x40) >> 6;
    n->cache_prog_support   = (nand_id->id3 & 0x80) >> 7;
    
    /* extract information from the 4th ID */
    n->page_size            = 1024 << (nand_id->id4 & 0x03);
    n->block_size           = (64*1024) << ((nand_id->id4 & 0x30) >> 4);
    n->spare_size_per_512   = 8 << ((nand_id->id4 & 0x04) >> 2);
    n->organization         = 8 << ((nand_id->id4 & 0x40) >> 6);
    n->serial_access_min    = 50 >> ((nand_id->id4 & 0x80) >> 7);
    
    /* extract information from the 5th ID */
    n->plane_number         = 1 << ((nand_id->id5 & 0x0c) >> 2);
    n->plane_size           = (8*1024*1024) << ((nand_id->id5 & 0x70) >> 4);

#if 0
    printf("internal chip number             = %d\r\n",            (int)n->internal_chip_number);
    printf("cell type                        = %d level cell\r\n", (int)n->cell_type);
    printf("simultaneously programmed pages  = %d\r\n",            (int)n->simul_prog_pages);
    printf("support interleave               = %d\r\n",            (int)n->interleave_support);
    printf("support cache program            = %d\r\n",            (int)n->cache_prog_support);

    printf("page size                        = %dKB\r\n",          (int)n->page_size >> 10);
    printf("block size                       = %dKB\r\n",          (int)n->block_size >> 10); 
    printf("spare area size / 512B           = %d\r\n",            (int)n->spare_size_per_512);
    printf("organization                     = x%d\r\n",           (int)n->organization);
    printf("serial access min                = %dns\r\n",          (int)n->serial_access_min);

    printf("plane number                     = %d\r\n",            (int)n->plane_number);
    printf("plane size                       = %dMB\r\n",          (int)n->plane_size >> 20);
    printf("--------------------------------------------------\r\n");
#endif

#if 1
    do {
        flash_chip_spec_t *f = &flash_spec[chip];

        f->data_size          = n->page_size;
        f->sectors_per_page   = n->page_size >> 9;
        f->spare_size         = n->spare_size_per_512 * f->sectors_per_page;
        f->page_size          = f->data_size + f->spare_size;
        f->block_size         = n->block_size;
        f->pages_per_block    = f->block_size / f->data_size;
        f->num_blocks         = n->plane_size / f->block_size * n->plane_number;
        f->num_dies_per_ce    = n->internal_chip_number;
        f->num_planes         = n->plane_number;
        f->max_num_bad_blocks = f->num_blocks * 245 / 10000;    /* 2.45% */

#if 1
        printf("flash_spec[%d].page_size          = %d\r\n", (int)chip, (int)f->page_size);
        printf("flash_spec[%d].data_size          = %d\r\n", (int)chip, (int)f->data_size);
        printf("flash_spec[%d].spare_size         = %d\r\n", (int)chip, (int)f->spare_size);
        printf("flash_spec[%d].sectors_per_page   = %d\r\n", (int)chip, (int)f->sectors_per_page);
        printf("flash_spec[%d].pages_per_block    = %d\r\n", (int)chip, (int)f->pages_per_block);
        printf("flash_spec[%d].block_size         = %d\r\n", (int)chip, (int)f->block_size);
        printf("flash_spec[%d].num_blocks         = %d\r\n", (int)chip, (int)f->num_blocks);
        printf("flash_spec[%d].num_dies_per_ce    = %d\r\n", (int)chip, (int)f->num_dies_per_ce);
        printf("flash_spec[%d].num_planes         = %d\r\n", (int)chip, (int)f->num_planes); 
        printf("flash_spec[%d].max_num_bad_blocks = %d\r\n", (int)chip, (int)f->max_num_bad_blocks);
        printf("--------------------------------------------------\r\n");
#endif
    } while (0);
#endif

    return(FM_SUCCESS);
}


/*----------------------------------------------------------------------*/
/*  Local Function Definitions                                          */
/*----------------------------------------------------------------------*/

static uint32_t fsmc_nand_reset(uint16_t chip)
{
    *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_RESET;

    return(NAND_READY);
}


static uint32_t fsmc_nand_get_status(uint16_t chip)
{
    uint32_t data, timeout = 2, status = NAND_BUSY;

    WAIT_FLASH_READY();

    /* wait for a NAND operation to complete or a TIMEOUT to occur */
    while ((status != NAND_READY) && (timeout != 0)) {

        /* send the read status command and read NAND status */
        *(vu8 *)(NAND_BASE_ADDR | CMD_AREA) = NAND_CMD_STATUS;
        data = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);

        if ((data & NAND_ERROR) == NAND_ERROR) {
            status = NAND_ERROR;
        } 
        else if ((data & NAND_READY) == NAND_READY) {
            status = NAND_READY;
        }
        else {
            status = NAND_BUSY; 
        }
        
        timeout--;      
    }
    
    if (timeout == 0) {          
        status = NAND_TIMEOUT_ERROR;      
    } 
    
    /* return the operation status */
    return (status);      
}


/*----------------------------------------------------------------------*/
/*  Data Transfer Functions (Memory <--> NAND Flash)                    */
/*----------------------------------------------------------------------*/

static void data_read(uint8_t *buf, uint32_t size)
{
    uint32_t i;

    for (i = 0; (uint32_t)(buf + i) & 0x03; i++) {
        buf[i] = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);
    }
    
    for ( ; i < size - 3; i += 4) {
        *(uint32_t *)(buf + i) = *(vu32 *)(NAND_ATTR_ADDR | DATA_AREA);
    }
    
    for ( ; i < size; i++) {
        buf[i] = *(vu8 *)(NAND_ATTR_ADDR | DATA_AREA);
    }
}


static void data_read_dma(uint8_t *buf, uint32_t size)
{
    DMA_InitTypeDef  DMA_InitStructure;
    uint32_t         timeout;
    bool_t           word_aligned = TRUE;
    static bool_t    dma_inited = FALSE;
    static uint32_t  dma_reg_cr = 0;
    
    if ((uint32_t)buf & 0x03) word_aligned = FALSE;
    if (size & 0x03) word_aligned = FALSE;
        
    while (DMA_GetCmdStatus(DMA_RD_STREAM) != DISABLE);

    if (! dma_inited) {
        DMA_InitStructure.DMA_Channel            = DMA_RD_CHANNEL;
        DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)(NAND_ATTR_ADDR | DATA_AREA);
        DMA_InitStructure.DMA_Memory0BaseAddr    = (uint32_t)buf;
        DMA_InitStructure.DMA_DIR                = DMA_DIR_MemoryToMemory;
        
        if (word_aligned) {
            DMA_InitStructure.DMA_BufferSize         = (size >> 2);
            DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
            DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Word;
        }
        else {
            DMA_InitStructure.DMA_BufferSize         = size;
            DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
            DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
        }
        
        DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
        DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;
        DMA_InitStructure.DMA_Mode               = DMA_Mode_Normal;
        DMA_InitStructure.DMA_Priority           = DMA_Priority_VeryHigh;
        DMA_InitStructure.DMA_FIFOMode           = DMA_FIFOMode_Enable;
        DMA_InitStructure.DMA_FIFOThreshold      = DMA_FIFOThreshold_Full;
        DMA_InitStructure.DMA_MemoryBurst        = DMA_MemoryBurst_Single;
        DMA_InitStructure.DMA_PeripheralBurst    = DMA_PeripheralBurst_Single;
    
        DMA_Init(DMA_RD_STREAM, &DMA_InitStructure);
        DMA_ITConfig(DMA_RD_STREAM, DMA_IT_TC, DISABLE);
        
        dma_inited = TRUE;
        dma_reg_cr = DMA_RD_STREAM->CR;
    }
    else {
        DMA_RD_STREAM->M0AR = (uint32_t)buf;
        DMA_RD_STREAM->CR   = dma_reg_cr;
        DMA_RD_STREAM->CR  &= DMA_DATA_SIZE_MASK;
        
        if (word_aligned) {
            DMA_RD_STREAM->CR  |= DMA_PeripheralDataSize_Word;
            DMA_RD_STREAM->CR  |= DMA_MemoryDataSize_Word;
            DMA_RD_STREAM->NDTR = (size >> 2);
        }
        else {
            DMA_RD_STREAM->CR  |= DMA_PeripheralDataSize_Byte;
            DMA_RD_STREAM->CR  |= DMA_MemoryDataSize_Byte;
            DMA_RD_STREAM->NDTR = size;
        }
    }
    
    DMA_ClearFlag(DMA_RD_STREAM, DMA_RD_TCIF);
    DMA_Cmd(DMA_RD_STREAM, ENABLE);

    timeout = 0x100000;
    while ((DMA_GetCmdStatus(DMA_RD_STREAM) != ENABLE) && (--timeout > 0));
    if (timeout == 0) {
        printf("[LLD] %s: DMA command timeout (buf = %p, size = %d) !!\r\n", 
               __FUNCTION__, buf, (int)size);
    }
    else {
        timeout = 0x100000;
        while ((DMA_GetFlagStatus(DMA_RD_STREAM, DMA_RD_TCIF) != SET) && (--timeout > 0));
        if (timeout == 0) {
            printf("[LLD] %s: DMA completion timeout (buf = %p, size = %d) !!\r\n", 
                   __FUNCTION__, buf, (int)size);
        }
        else {
            //printf("[LLD] %s: DMA completion OK (timeout = %08x) !!\r\n", 
            //       __FUNCTION__, (unsigned int)timeout);
        }
    }
}


static void data_write(uint8_t *buf, uint32_t size)
{
    uint32_t i;
    
    for (i = 0; (uint32_t)(buf + i) & 0x03; i++) {
        *(vu8 *)(NAND_BASE_ADDR | DATA_AREA) = buf[i];
    }
    
    for ( ; i < size - 3; i += 4) {
        *(vu32 *)(NAND_BASE_ADDR | DATA_AREA) = *(uint32_t *)(buf + i);
    }
    
    for ( ; i < size; i++) {
        *(vu8 *)(NAND_BASE_ADDR | DATA_AREA) = buf[i];
    }
}


static void data_write_dma(uint8_t *buf, uint32_t size)
{
    DMA_InitTypeDef  DMA_InitStructure;
    uint32_t         timeout;
    bool_t           word_aligned = TRUE;
    static bool_t    dma_inited = FALSE;
    static uint32_t  dma_reg_cr = 0;
    
    if ((uint32_t)buf & 0x03) word_aligned = FALSE;
    if (size & 0x03) word_aligned = FALSE;
    
    while (DMA_GetCmdStatus(DMA_WR_STREAM) != DISABLE);

    if (! dma_inited) {
        DMA_InitStructure.DMA_Channel            = DMA_WR_CHANNEL;
        DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)buf;
        DMA_InitStructure.DMA_Memory0BaseAddr    = (uint32_t)(NAND_BASE_ADDR | DATA_AREA);
        DMA_InitStructure.DMA_DIR                = DMA_DIR_MemoryToMemory;
        
        if (word_aligned) {
            DMA_InitStructure.DMA_BufferSize         = (size >> 2);
            DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
            DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Word;
        }
        else {
            DMA_InitStructure.DMA_BufferSize         = size;
            DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
            DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
        }
        
        DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Enable;
        DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Disable;
        DMA_InitStructure.DMA_Mode               = DMA_Mode_Normal;
        DMA_InitStructure.DMA_Priority           = DMA_Priority_VeryHigh;
        DMA_InitStructure.DMA_FIFOMode           = DMA_FIFOMode_Enable;
        DMA_InitStructure.DMA_FIFOThreshold      = DMA_FIFOThreshold_Full;
        DMA_InitStructure.DMA_MemoryBurst        = DMA_MemoryBurst_Single;
        DMA_InitStructure.DMA_PeripheralBurst    = DMA_PeripheralBurst_Single;
    
        DMA_Init(DMA_WR_STREAM, &DMA_InitStructure);
        DMA_ITConfig(DMA_WR_STREAM, DMA_IT_TC, DISABLE);
        
        dma_inited = TRUE;
        dma_reg_cr = DMA_WR_STREAM->CR;
    }
    else {
        DMA_WR_STREAM->PAR = (uint32_t)buf;
        DMA_WR_STREAM->CR  = dma_reg_cr;
        DMA_WR_STREAM->CR &= DMA_DATA_SIZE_MASK;

        if (word_aligned) {
            DMA_WR_STREAM->CR  |= DMA_PeripheralDataSize_Word;
            DMA_WR_STREAM->CR  |= DMA_MemoryDataSize_Word;
            DMA_WR_STREAM->NDTR = (size >> 2);
        }
        else {
            DMA_WR_STREAM->CR  |= DMA_PeripheralDataSize_Byte;
            DMA_WR_STREAM->CR  |= DMA_MemoryDataSize_Byte;
            DMA_WR_STREAM->NDTR = size;
        }
    }
    
    DMA_ClearFlag(DMA_WR_STREAM, DMA_WR_TCIF);
    DMA_Cmd(DMA_WR_STREAM, ENABLE);

    timeout = 0x100000;
    while ((DMA_GetCmdStatus(DMA_WR_STREAM) != ENABLE) && (--timeout > 0));
    if (timeout == 0) {
        printf("[LLD] %s: DMA command timeout (buf = %p, size = %d) !!\r\n", 
               __FUNCTION__, buf, (int)size);
    }
    else {
        timeout = 0x100000;
        while ((DMA_GetFlagStatus(DMA_WR_STREAM, DMA_WR_TCIF) != SET) && (--timeout > 0));
        if (timeout == 0) {
            printf("[LLD] %s: DMA completion timeout (buf = %p, size = %d) !!\r\n", 
                   __FUNCTION__, buf, (int)size);
        }
        else {
            //printf("[LLD] %s: DMA completion OK (timeout = %08x) !!\r\n", 
            //       __FUNCTION__, (unsigned int)timeout);
        }
    }
}


/*----------------------------------------------------------------------*/
/*  ECC Correction Function                                             */
/*----------------------------------------------------------------------*/

#if (ECC_METHOD == HW_ECC)

#define ECC_MASK28      0x0FFFFFFF      /* 28 valid ECC parity bits */
#define ECC_MASK        0x05555555      /* 14 ECC parity bits       */

static int32_t ecc_correct_data(uint32_t ecc_calc, uint32_t ecc_read, uint8_t *data)
{
    uint32_t count, bit_num, byte_addr;
    uint32_t mask, syndrome;
    uint32_t ecc_even;                  /* 14 even ECC parity bits */
    uint32_t ecc_odd;                   /* 14 odd ECC parity bits  */

    ecc_calc ^= 0xFFFFFFFF;
    ecc_read ^= 0xFFFFFFFF;
    syndrome = (ecc_calc ^ ecc_read) & ECC_MASK28;

    if (syndrome == 0) {
        return(ECC_NO_ERROR);                   /* no bit-flip errors in data */
    }

    ecc_odd  = syndrome & ECC_MASK;             /* get 14 odd parity bits */
    ecc_even = (syndrome >> 1) & ECC_MASK;      /* get 14 even parity bits */

    if ((ecc_odd ^ ecc_even) == ECC_MASK) {     /* 1-bit correctable error? */
        bit_num = (ecc_even & 0x01) |
                  ((ecc_even >> 1) & 0x02) |
                  ((ecc_even >> 2) & 0x04);

        byte_addr = ((ecc_even >> 6) & 0x001) |
                    ((ecc_even >> 7) & 0x002) |
                    ((ecc_even >> 8) & 0x004) |
                    ((ecc_even >> 9) & 0x008) |
                    ((ecc_even >> 10) & 0x010) |
                    ((ecc_even >> 11) & 0x020) |
                    ((ecc_even >> 12) & 0x040) |
                    ((ecc_even >> 13) & 0x080) |
                    ((ecc_even >> 14) & 0x100) |
                    ((ecc_even >> 15) & 0x200) |
                    ((ecc_even >> 16) & 0x400) ;

        data[byte_addr] ^= (1 << bit_num);

        return(ECC_CORRECTABLE_ERROR);          /* correction succeeded */
    }

    /* count the number of 1's in the syndrome */
    count = 0;
    mask  = 0x00800000;
    while (mask) {
        if (syndrome & mask) count++;
        mask >>= 1;
    }

    if (count == 1) {                   /* error in the ECC itself */
        return(ECC_ECC_ERROR);
    }

    return(ECC_UNCORRECTABLE_ERROR);    /* unable to correct data */
}

#endif /* (ECC_METHOD == HW_ECC) */
#endif /* CFD_M */
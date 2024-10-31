/**
 * @file port.c
 * @author SprInec (JulyCub@163.com)
 * @brief 
 * @version 0.1
 * @date 2024.10.07
 * 
 * @copyright Copyright (c) 2024
 * 
 */

#include "FreeRTOS.h"
#include "task.h"
#include "ARMCM4.h"

#define portINITIAL_XPSR (0x01000000)
#define portSTART_ADDRESS_MASK ((StackType_t)0xfffffffeUL)

#define portNVIC_SYSPRI2_REG (*((volatile uint32_t *)0xE000ED20))

#define portNVIC_PENDSV_PRI (((uint32_t)configKERNEL_INTERRUPT_PRIORITY) << 16UL)
#define portNVIC_SYSTICK_PRI (((uint32_t)configKERNEL_INTERRUPT_PRIORITY) << 24UL)


void prvStartFirstTask( void );
void vPortSVCHandler( void );
void xPortPendSVHandler( void );

static void prvTaskExitError(void)
{
    /* 函数停止在这里 */
    for (;;);
}

/**
 * @brief 
 * 
 * @param pxTopOfStack 
 * @param pxCode 
 * @param pvParameters 
 * @return StackType_t* 
 */
StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters)
{
    /* 异常发生时, 自动加载到 CPU 寄存器的内容 */
    pxTopOfStack--;
    *pxTopOfStack = portINITIAL_XPSR;
    pxTopOfStack--;
    *pxTopOfStack = (StackType_t)pxCode & portSTART_ADDRESS_MASK;
    pxTopOfStack--;
    *pxTopOfStack = (StackType_t)prvTaskExitError;
    pxTopOfStack -= 5; /* R12 R3 R2 and R1 默认初始化为0 */
    *pxTopOfStack = (StackType_t)pvParameters;

    /* 异常发生时, 手动加载到 CPU 寄存器的内容 */
    pxTopOfStack -= 8;

    /* 返回栈顶指针, 此时pxTopOfStack指向空闲栈 */
    return pxTopOfStack;
}

BaseType_t xPortStartScheduler(void)
{
    /* 配置 PendSV 和 SysTick 的中断优先级为最低 */
    portNVIC_SYSPRI2_REG |= portNVIC_PENDSV_PRI;
    portNVIC_SYSPRI2_REG |= portNVIC_SYSTICK_PRI;

    /* 启动第一个任务, 不再返回 */
    prvStartFirstTask();

    /* 不应运行到这里 */
    return 0;
}

#if (COMPILER_VERSION == 5)
__asm void prvStartFirstTask( void )
{
	PRESERVE8

	/* 在Cortex-M中，0xE000ED08是SCB_VTOR这个寄存器的地址，
       里面存放的是向量表的起始地址，即MSP的地址 */
	ldr r0, =0xE000ED08
	ldr r0, [r0]
	ldr r0, [r0]

	/* 设置主堆栈指针msp的值 */
	msr msp, r0
    
	/* 使能全局中断 */
	cpsie i
	cpsie f
	dsb
	isb
	
    /* 调用SVC去启动第一个任务 */
	svc 0  
	nop
	nop
}

__asm void vPortSVCHandler( void )
{
    extern pxCurrentTCB;
    
    PRESERVE8

	ldr	r3, =pxCurrentTCB	/* 加载pxCurrentTCB的地址到r3 */
	ldr r1, [r3]			/* 加载pxCurrentTCB到r1 */
	ldr r0, [r1]			/* 加载pxCurrentTCB指向的值到r0，目前r0的值等于第一个任务堆栈的栈顶 */
	ldmia r0!, {r4-r11}		/* 以r0为基地址，将栈里面的内容加载到r4~r11寄存器，同时r0会递增 */
	msr psp, r0				/* 将r0的值，即任务的栈指针更新到psp */
	isb
	mov r0, #0              /* 设置r0的值为0 */
	msr	basepri, r0         /* 设置basepri寄存器的值为0，即所有的中断都没有被屏蔽 */
	orr r14, #0xd           /* 当从SVC中断服务退出前,通过向r14寄存器最后4位按位或上0x0D，
                               使得硬件在退出时使用进程堆栈指针PSP完成出栈操作并返回后进入线程模式、返回Thumb状态 */
    
	bx r14                  /* 异常返回，这个时候栈中的剩下内容将会自动加载到CPU寄存器：
                               xPSR，PC（任务入口地址），R14，R12，R3，R2，R1，R0（任务的形参）
                               同时PSP的值也将更新，即指向任务栈的栈顶 */
}

__asm void xPortPendSVHandler( void )
{
	extern pxCurrentTCB;
	extern vTaskSwitchContext;

	PRESERVE8

    /* 当进入PendSVC Handler时，上一个任务运行的环境即：
       xPSR，PC（任务入口地址），R14，R12，R3，R2，R1，R0（任务的形参）
       这些CPU寄存器的值会自动保存到任务的栈中，剩下的r4~r11需要手动保存 */
    /* 获取任务栈指针到r0 */
	mrs r0, psp
	isb

	ldr	r3, =pxCurrentTCB		/* 加载pxCurrentTCB的地址到r3 */
	ldr	r2, [r3]                /* 加载pxCurrentTCB到r2 */

	stmdb r0!, {r4-r11}			/* 将CPU寄存器r4~r11的值存储到r0指向的地址 */
	str r0, [r2]                /* 将任务栈的新的栈顶指针存储到当前任务TCB的第一个成员，即栈顶指针 */				
                               

	stmdb sp!, {r3, r14}        /* 将R3和R14临时压入堆栈，因为即将调用函数vTaskSwitchContext,
                                  调用函数时,返回地址自动保存到R14中,所以一旦调用发生,R14的值会被覆盖,因此需要入栈保护;
                                  R3保存的当前激活的任务TCB指针(pxCurrentTCB)地址,函数调用后会用到,因此也要入栈保护 */
	mov r0, #configMAX_SYSCALL_INTERRUPT_PRIORITY    /* 进入临界段 */
	msr basepri, r0
	dsb
	isb
	bl vTaskSwitchContext       /* 调用函数vTaskSwitchContext，寻找新的任务运行,通过使变量pxCurrentTCB指向新的任务来实现任务切换 */ 
	mov r0, #0                  /* 退出临界段 */
	msr basepri, r0
	ldmia sp!, {r3, r14}        /* 恢复r3和r14 */

	ldr r1, [r3]
	ldr r0, [r1] 				/* 当前激活的任务TCB第一项保存了任务堆栈的栈顶,现在栈顶值存入R0*/
	ldmia r0!, {r4-r11}			/* 出栈 */
	msr psp, r0
	isb
	bx r14                      /* 异常发生时,R14中保存异常返回标志,包括返回后进入线程模式还是处理器模式、
                                   使用PSP堆栈指针还是MSP堆栈指针，当调用 bx r14指令后，硬件会知道要从异常返回，
                                   然后出栈，这个时候堆栈指针PSP已经指向了新任务堆栈的正确位置，
                                   当新任务的运行地址被出栈到PC寄存器后，新的任务也会被执行。*/
	nop
}
#elif (COMPILER_VERSION == 6)
void prvStartFirstTask(void)
{
    __asm volatile(".p2align 3");

    __asm("ldr r0, = 0xE000ED08");
    __asm("ldr r0, [r0]");
    __asm("ldr r0, [r0]");

    __asm("msr msp, r0");

    __asm("cpsie i");
    __asm("cpsie f");

    __asm("dsb");
    __asm("isb");

    __asm("svc 0");
    __asm("nop");
    __asm("nop");
}

void vPortSVCHandler(void)
{
    extern TCB_t *pxCurrentTCB;

    __asm volatile(".p2align 3");

    __asm("ldr r3, =pxCurrentTCB");
    __asm("ldr r1, [r3]");
    __asm("ldr r0, [r1]");
    __asm("ldmia r0!, {r4-r11}");
    __asm("msr psp, r0");
    __asm("isb");
    __asm("mov r0, #0");
    __asm("msr basepri, r0");
    __asm("orr r14, #0xd");

    __asm("bx r14");
}

/**
 * @brief 实现任务切换 
 */
void xPortPendSVHandler(void)
{
    extern TCB_t *pxCurrentTCB;
    extern void vTaskSwitchContext(void);

    __asm volatile(".p2align 3");

    __asm("mrs r0, psp");
    __asm("isb");

    __asm("ldr r3, =pxCurrentTCB");
    __asm("ldr r2, [r3]");

    __asm("stmdb r0!, {r4-r11}");
    __asm("str r0, [r2]");

    __asm("stmdb sp!, {r3, r4}");
    __asm volatile ("mov r0, %0" :: "i" (configMAX_SYSCALL_INTERRUPT_PRIORITY));
    __asm("msr basepri, r0");
    __asm("dsb");
    __asm("isb");
    __asm("bl vTaskSwitchContext");
    __asm("mov r0, #0");
    __asm("msr basepri, r0");
    __asm("ldmia sp!, {r3, r14}");

    __asm("ldr r1, [r3]");
    __asm("ldr r0, [r1]");
    __asm("ldmia r0!, {r4-r11}");
    __asm("msr psp, r0");
    __asm("isb");
    __asm("bx r14");
    __asm("nop");
}
#endif /* COMPILER_VERSION */
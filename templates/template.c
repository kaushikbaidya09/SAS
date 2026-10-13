//=============================================================================
/*!
    \file    template.c
    \brief   <One-line description of what this module implements.>
*/
//=============================================================================

#include "template.h"

#include <string.h>

#define TEMPLATE_READY_TIMEOUT (10000UL)  ///< HW ready timeout, in loop iterations.
#define TEMPLATE_BAUD_MIN      (1200UL)   ///< Minimum supported baud rate in bits/s.
#define TEMPLATE_BAUD_MAX      (115200UL) ///< Maximum supported baud rate in bits/s.

/*!
    \brief Internal state machine states.
*/
typedef enum
{
    TEMPLATE_STATE_UNINIT = 0, ///< Module not yet initialised.
    TEMPLATE_STATE_IDLE,       ///< Initialised and waiting for work.
    TEMPLATE_STATE_BUSY,       ///< Transfer in progress.
    TEMPLATE_STATE_FAULT       ///< Unrecoverable error; needs re-init.
} template_state_t;

/*!
    \brief Module control block (all private run-time data in one place).
*/
typedef struct
{
    template_config_t config; ///< Copy of the active configuration.
    template_state_t state;   ///< Current state machine state.
    uint32_t err_cnt;         ///< Number of errors since init.
} template_ctrl_t;

static template_ctrl_t g_ctrl; ///< Module control block.

static volatile bool g_b_xfer_done = false; ///< Transfer complete flag, set by ISR.

static bool template_is_config_valid(const template_config_t *p_config);
static template_status_t template_hw_setup(const template_config_t *p_config);

// ISR prototype; normally provided by the vendor CMSIS device header.
void TEMPLATE_IRQHandler(void);

/*!
    \brief   Initialise the module.
    \param[in] p_config  Pointer to configuration structure. Must not be NULL.
    \return  TEMPLATE_OK on success, otherwise a TEMPLATE_ERR_xxx code.
*/
template_status_t template_init(const template_config_t *p_config)
{
    template_status_t status = TEMPLATE_OK; // Single exit point (MISRA 15.5)

    if (TEMPLATE_STATE_UNINIT != g_ctrl.state)
    {
        status = TEMPLATE_ERR_STATE;
    }
    else if (!template_is_config_valid(p_config))
    {
        status = TEMPLATE_ERR_PARAM;
    }
    else
    {
        status = template_hw_setup(p_config);

        if (TEMPLATE_OK == status)
        {
            // Keep a private copy so the caller's struct may go out of scope.
            (void)memcpy(&g_ctrl.config, p_config, sizeof(g_ctrl.config));
            g_ctrl.err_cnt = 0UL;
            g_ctrl.state   = TEMPLATE_STATE_IDLE;
        }
    }

    return status;
}

/*!
    \brief   De-initialise the module and release its hardware.
    \return  TEMPLATE_OK on success, TEMPLATE_ERR_STATE if not initialised.
*/
template_status_t template_deinit(void)
{
    template_status_t status = TEMPLATE_OK;

    if (TEMPLATE_STATE_UNINIT == g_ctrl.state)
    {
        status = TEMPLATE_ERR_STATE;
    }
    else
    {
        // TODO: disable peripheral clock, IRQ and release pins.
        (void)memset(&g_ctrl, 0, sizeof(g_ctrl));
        g_ctrl.state = TEMPLATE_STATE_UNINIT;
    }

    return status;
}

/*!
    \brief   Run one iteration of the module state machine.
*/
void template_process(void)
{
    switch (g_ctrl.state)
    {
        case TEMPLATE_STATE_IDLE:
            // Nothing pending.
            break;

        case TEMPLATE_STATE_BUSY:
            if (g_b_xfer_done)
            {
                g_b_xfer_done = false; // Acknowledge ISR flag.
                g_ctrl.state  = TEMPLATE_STATE_IDLE;
            }
            break;

        case TEMPLATE_STATE_FAULT:
            g_ctrl.err_cnt++;
            break;

        case TEMPLATE_STATE_UNINIT:
        default:
            // Not initialised or corrupted state: do nothing (MISRA 16.4).
            break;
    }
}

/*!
    \brief   Transfer-complete interrupt handler.
*/
void TEMPLATE_IRQHandler(void)
{
    // TODO: clear the peripheral interrupt flag first.
    g_b_xfer_done = true;
}

/*!
    \brief   Check a configuration structure for valid values.
*/
static bool template_is_config_valid(const template_config_t *p_config)
{
    bool b_valid = false;

    if (NULL != p_config)
    {
        b_valid = (p_config->channel < TEMPLATE_MAX_CHANNELS) &&
                  (p_config->baud_rate >= TEMPLATE_BAUD_MIN) &&
                  (p_config->baud_rate <= TEMPLATE_BAUD_MAX);
    }

    return b_valid;
}

/*!
    \brief   Configure the peripheral registers.
*/
static template_status_t template_hw_setup(const template_config_t *p_config)
{
    template_status_t status = TEMPLATE_OK;
    uint32_t timeout         = TEMPLATE_READY_TIMEOUT;

    (void)p_config; // TODO: remove once registers are written.

    // TODO: enable clock, write baud-rate divisor, enable peripheral.

    // Bounded wait: never spin forever on a hardware flag.
    // TODO: replace false with !PERIPH->SR.READY
    while ((timeout > 0UL) && (false))
    {
        timeout--;
    }

    if (0UL == timeout)
    {
        status = TEMPLATE_ERR_HW;
    }

    return status;
}

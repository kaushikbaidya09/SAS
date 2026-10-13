//=============================================================================
/*!
    \file    template.h
    \brief   <One-line description of the module's public interface.>
*/
//=============================================================================

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <stdint.h>
#include <stdbool.h>

#define TEMPLATE_MAX_CHANNELS (4U) ///< Number of channels supported.

/*!
    \brief Return/status codes used by this module.
*/
typedef enum
{
    TEMPLATE_OK = 0,      ///< Operation completed successfully.
    TEMPLATE_ERR_PARAM,   ///< Invalid argument (NULL pointer, range).
    TEMPLATE_ERR_STATE,   ///< Called in the wrong module state.
    TEMPLATE_ERR_TIMEOUT, ///< Operation did not complete in time.
    TEMPLATE_ERR_HW       ///< Underlying hardware reported a fault.
} template_status_t;

/*!
    \brief Module configuration, passed to template_init().
*/
typedef struct
{
    uint32_t baud_rate; ///< Communication speed in bits/s.
    uint8_t channel;    ///< Channel index, 0..TEMPLATE_MAX_CHANNELS-1
    bool b_enable_irq;  ///< true = interrupt driven, false = polled.
} template_config_t;

template_status_t template_init(const template_config_t *p_config);
template_status_t template_deinit(void);
void template_process(void);

#endif /* TEMPLATE_H */

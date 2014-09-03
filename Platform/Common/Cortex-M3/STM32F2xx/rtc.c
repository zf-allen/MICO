/*
 * Copyright 2013, Broadcom Corporation
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
 * the contents of this file may not be disclosed to third parties, copied
 * or duplicated in any form, in whole or in part, without the prior
 * written permission of Broadcom Corporation.
 */
#include "stm32f2xx_platform.h"
#include "MICOPlatform.h"
#include "wwd_assert.h"
#include "rtc.h"

/******************************************************
 *                   Enumerations
 ******************************************************/

typedef enum
{
    CLOCKING_EVERY_SEC,
    CLOCKING_EVERY_1p25MSEC
}rtc_clock_state_t;


/******************************************************
 *               Function Declarations
 ******************************************************/

#if defined(WICED_ENABLE_MCU_RTC) && !defined(WICED_DISABLE_MCU_POWERSAVE)
static wiced_result_t     stm32f2_rtc_change_clock( rtc_clock_state_t* current, rtc_clock_state_t target );
static void               add_second_to_time( wiced_rtc_time_t* time );
static void               subtract_second_from_time( wiced_rtc_time_t* time );
static int                add_1p25ms_contribution( uint32_t ms, uint32_t* seconds_contribution );
static wiced_result_t     compensate_time_error( uint32_t sec, wiced_bool_t subtract );
static void               reset_rtc_values( void );
static uint32_t           convert_rtc_calendar_values_to_units_passed( void );
#endif /* #ifndef WICED_DISABLE_MCU_POWERSAVE */
/******************************************************
 *                      Macros
 ******************************************************/

#define LEAP_YEAR_OR_NOT(year)( ( year % 4 ) ? ( 0 ) : ( 1 ) )


/******************************************************
 *                    Constants
 ******************************************************/

const char not_leap_days[]=
{
    0,31,28,31,30,31,30,31,31,30,31,30,31
};

const char leap_days[]=
{
    0,31,29,31,30,31,30,31,31,30,31,30,31
};

#define LEAP_YEAR_DAY_COUNT         ( 366 )
#define NOT_LEAP_YEAR_DAY_COUNT     ( 365 )
#define NUM_SECONDS_IN_MINUTE       ( 60 )
#define NUM_SECONDS_IN_HOUR         ( 3600 )
#define NUM_1P25MS_IN_SEC           ( 800 )

#ifndef WICED_DISABLE_MCU_POWERSAVE

/******************************************************
 *               Variables Definitions
 ******************************************************/
wiced_rtc_time_t    saved_rtc_time;
rtc_clock_state_t   current_clock_state = CLOCKING_EVERY_SEC;
uint16_t            current_ms_left_over=0;
static int rtc_reset_fail = 0;

static uint32_t convert_rtc_calendar_values_to_units_passed( void )
{
    long int        temp1=0;
    long int        temp2=0;
    int             temp=0;
    long int        temp_days=0;
    uint8_t         current_year;
    RTC_TimeTypeDef rtc_read_time;
    RTC_DateTypeDef rtc_read_date;


    /* Read current rtc time */
    RTC_GetTime( RTC_Format_BIN, &rtc_read_time );
    RTC_GetDate( RTC_Format_BIN, &rtc_read_date );

    /* Calculate number of days in the previous years */
    if( rtc_read_date.RTC_Year != 0 )
    {
        for( temp = (int)( rtc_read_date.RTC_Year - 1 ); temp >= 0; temp-- )
        {
            temp_days += (LEAP_YEAR_OR_NOT(temp)) ? (LEAP_YEAR_DAY_COUNT): (NOT_LEAP_YEAR_DAY_COUNT);
        }
    }

    current_year = rtc_read_date.RTC_Year;
    wiced_assert("Inappropriate month value in RTC", (rtc_read_date.RTC_Month != 0) );
    if( rtc_read_date.RTC_Month != 0 )
    {
        /* Calculate number of days passed in the current year and add them to previous days value */
        for( temp = (int)( rtc_read_date.RTC_Month - 1 ); temp > 0; temp-- )
        {
            temp_days += LEAP_YEAR_OR_NOT(current_year)?(leap_days[temp]):(not_leap_days[temp]);
        }
    }

    /* Convert passed hours, seconds and minutes to seconds */
    temp1 = rtc_read_time.RTC_Seconds + rtc_read_time.RTC_Minutes*NUM_SECONDS_IN_MINUTE + rtc_read_time.RTC_Hours*NUM_SECONDS_IN_HOUR;

    wiced_assert("Inappropriate date value in RTC", ( rtc_read_date.RTC_Date != 0 ) );

    /* Convert passed days to seconds */
    if( rtc_read_date.RTC_Date != 0 )
    {
        temp2 = ( ( rtc_read_date.RTC_Date - 1 ) + temp_days ) * NUM_SECONDS_IN_HOUR * 24;
    }

    /* Return total number of seconds passed  */
    return (uint32_t)( temp1 + temp2 );

}


static void reset_rtc_values( void )
{
    ErrorStatus status;

    rtc_reset_fail = 0;
    /* Disable write protection of rtc registers */
    RTC_WriteProtectionCmd(DISABLE);
    status = RTC_EnterInitMode();
    REFERENCE_DEBUG_ONLY_VARIABLE(status);
    wiced_assert( "Rtc can not enter intialisation mode", status==SUCCESS )

    /* Reset calendar date registers */
    RTC->TR = 0;
    RTC_ExitInitMode();
    status = RTC_WaitForSynchro();
    wiced_assert( "Rtc can not synchronize", status==SUCCESS )

    /* Enable write protection of rtc registers */
    RTC_WriteProtectionCmd(ENABLE);

    /* Disable write protection of the rtc registers */
    RTC_WriteProtectionCmd(DISABLE);
    status = RTC_EnterInitMode();
    wiced_assert( "Rtc can not enter intialisation mode", status==SUCCESS )

    /* 2000 year 01/01 */
    RTC->TR = 0;
    RTC->DR= 0;
    RTC->DR= ( 1<<13 ) | ( 1<<8 ) | ( 1<<0 );
    RTC_ExitInitMode();
    status = RTC_WaitForSynchro();
    wiced_assert( "Rtc can not synchronize", status==SUCCESS );

    /* Enable write protection of rtc registers */
    RTC_WriteProtectionCmd(ENABLE);
    if (RTC->TR > 2000)
        rtc_reset_fail = 1;
}

static wiced_result_t stm32f2_rtc_change_clock( rtc_clock_state_t* current, rtc_clock_state_t target )
{
    uint8_t sync_div;
    uint8_t async_div;
    ErrorStatus status;

    /* Changing the synchronous and asynchronous prescalers according to the current clocking state and target_clock_state */
    /* of the RTC */
    if( *current == CLOCKING_EVERY_SEC )
    {
        if( target == CLOCKING_EVERY_1p25MSEC )
        {
            sync_div = 0;
            async_div= 40;

            /* Disable write protection of rtc registers, now we will be able to update RTC register values */
            RTC_WriteProtectionCmd(DISABLE);

            /* Enable initialisation mode */
            status = RTC_EnterInitMode();
            REFERENCE_DEBUG_ONLY_VARIABLE(status);
            wiced_assert( "Rtc can not enter intialisation mode", status==SUCCESS )

            /* Update RTC prescaler */
            RTC->PRER = (uint32_t)( sync_div );
            RTC->PRER |= (uint32_t)(async_div << 16);
            RTC_ExitInitMode();

            /* Enable write proteciton of rtc registers back */
            RTC_WriteProtectionCmd(ENABLE);
            *current = CLOCKING_EVERY_1p25MSEC;
            return WICED_SUCCESS;
        }
        else
        {
            return WICED_SUCCESS;
        }
    }
    else if( *current == CLOCKING_EVERY_1p25MSEC )
    {
        if( target == CLOCKING_EVERY_SEC )
        {
            /* We will get approximately 800Hz clock, 1 tick will correspond to 1.25ms(0,00125s), by addind 8 together we will get 1ms  */
            /* the closest division factor which is 40.96( 32768/800 ), we will take 41, the error will be very very tiny */
            async_div = 127;
            sync_div = 255;

            /* Disable write protection of rtc registers */
            RTC_WriteProtectionCmd(DISABLE);

            /* Enable initialisation mode */
            status = RTC_EnterInitMode();
            wiced_assert( "Rtc can not enter intialisation mode", status==SUCCESS )

            /* Update RTC prescaler */
            RTC->PRER = (uint32_t)( sync_div );
            RTC->PRER |= (uint32_t)(async_div << 16);

            RTC_ExitInitMode();

            /* Enable write proteciton of rtc registers back */
            RTC_WriteProtectionCmd(ENABLE);
            *current = CLOCKING_EVERY_SEC;
            return WICED_SUCCESS;
        }
        else
        {
            return WICED_SUCCESS;
        }
    }
    return WICED_SUCCESS;
}

static wiced_result_t platform_get_rtc_time(wiced_rtc_time_t* time)
{
    RTC_TimeTypeDef rtc_read_time;
    RTC_DateTypeDef rtc_read_date;

    if( time == NULL )
    {
        return WICED_BADARG;
    }

    /* save current rtc time */
    RTC_GetTime( RTC_Format_BIN, &rtc_read_time );
    RTC_GetDate( RTC_Format_BIN, &rtc_read_date );

    /* fill structure */
    time->sec = rtc_read_time.RTC_Seconds;
    time->min = rtc_read_time.RTC_Minutes;
    time->hr = rtc_read_time.RTC_Hours;
    time->weekday = rtc_read_date.RTC_WeekDay;
    time->date = rtc_read_date.RTC_Date;
    time->month= rtc_read_date.RTC_Month;
    time->year = rtc_read_date.RTC_Year;

    return WICED_SUCCESS;
}


#ifdef WICED_ENABLE_MCU_RTC /* link this function only when powersave is enabled and RTC is enabled at the same time */

static void add_second_to_time( wiced_rtc_time_t* time )
{
    if ( time->sec == 59 )
    {
        if ( time->min == 59 )
        {
            if ( time->hr == 23 )
            {
                if( time->date == ( LEAP_YEAR_OR_NOT(time->year) ? leap_days[time->month] :not_leap_days[time->month] ) )
                {
                    if( time->month == 12 )
                    {
                        /* Adding one second leads to year increment */
                        time->year++;
                        time->month=1;
                        time->date=1;
                        time->hr=0;
                        time->min=0;
                        time->sec=0;
                        if( time->weekday == 7 )
                        {
                            time->weekday=1;
                        }
                        else
                        {
                            time->weekday++;
                        }
                    }
                    else
                    {
                        /* Adding one seconds leads to month increment */
                        time->month++;
                        time->date=1;
                        time->hr=0;
                        time->min=0;
                        time->sec=0;
                        if( time->weekday == 7 )
                        {
                            time->weekday=1;
                        }
                        else
                        {
                            time->weekday++;
                        }
                    }
                }
                else
                {
                    /* Adding one seconds leads to data increment */
                    if ( time->weekday == 7 )
                    {
                        time->weekday=1;
                    }
                    else
                    {
                        time->weekday++;
                    }
                    time->date++;
                    time->hr=0;
                    time->min=0;
                    time->sec=0;
                }
            }
            else
            {
                /* Adding one seconds leads to hour increment */
                time->hr++;
                time->min=0;
                time->sec=0;
            }
        }
        else
        {
            /* Adding one seconds leads to minute increment */
            time->min++;
            time->sec=0;
        }
    }
    else
    {
        time->sec++;
    }
}

static void subtract_second_from_time( wiced_rtc_time_t* time )
{
    if ( time->sec == 0 )
    {
        if ( time->min == 0 )
        {
            if ( time->hr == 0 )
            {
                if( time->date == 1 )
                {
                    if( time->month == 1 )
                    {
                        /* Subtracting one second leads to year decrement */
                        time->year--;
                        time->month=12;
                        time->date=(uint8_t)( LEAP_YEAR_OR_NOT(time->year) ? leap_days[time->month] :not_leap_days[time->month] );
                        time->hr=23;
                        time->min=59;
                        time->sec=59;
                        if(time->weekday == 1)
                        {
                            time->weekday=7;
                        }
                        else
                        {
                            time->weekday--;
                        }
                    }
                    else
                    {
                        /* Subtracting one second leads to month decrement */
                        time->month--;
                        time->date=(uint8_t)( LEAP_YEAR_OR_NOT(time->year) ? leap_days[time->month] :not_leap_days[time->month] );
                        time->hr=23;
                        time->min=59;
                        time->sec=59;
                        if(time->weekday == 1)
                        {
                            time->weekday=7;
                        }
                        else
                        {
                            time->weekday--;
                        }
                    }
                }
                else
                {
                    /* Subtracting one second leads to date decrement */
                    if ( time->weekday == 1 )
                    {
                        time->weekday=7;
                    }
                    else
                    {
                        time->weekday--;
                    }
                    time->date--;
                    time->hr=23;
                    time->min=59;
                    time->sec=59;
                }
            }
            else
            {
                /* Subtracting one second leads to hour decrement */
                time->hr--; /*  */
                time->min=59;
                time->sec=59;
            }
        }
        else
        {
            /* Subtracting one second leads to minute decrement */
            time->min--;
            time->sec=59;
        }
    }
    else
    {
        time->sec--;
    }
}

static wiced_result_t compensate_time_error( uint32_t sec, wiced_bool_t subtract )
{
    if( subtract == WICED_FALSE )
    {
        /* Adding seconds to time */
        for( sec=sec ; sec > 0; sec--)
        {
            add_second_to_time(&saved_rtc_time);
        }
    }
    else
    {
        /* Subtracting seconds from time */
        for( sec=sec ; sec > 0; sec-- )
        {
            subtract_second_from_time(&saved_rtc_time);
        }
    }
    return WICED_SUCCESS;
}

static int add_1p25ms_contribution( uint32_t units_1p25ms, uint32_t* seconds_contribution )
{
    float temp;
    temp = (float)units_1p25ms*(float)1.25;

    /* Get number of ticks converts them to seconds and returns, number of seconds that were contributed to the */
    /* current time value. This value will be required by compensation algorithms */
    *seconds_contribution = (uint32_t)temp / 1000;
    if(*seconds_contribution)
    {
        compensate_time_error(*seconds_contribution, WICED_FALSE);
    }


    /* Returns leftover of 1.25 ms units, that are still remaining to contribute to a second */
    return (int)(units_1p25ms % NUM_1P25MS_IN_SEC);
}

#endif /* #ifdef WICED_ENABLE_MCU_RTC */


wiced_result_t rtc_sleep_entry ( void )
{

//#ifdef RTC_ENABLED /* !!If we dont read the time and store it. get an error while trying to enter STM RTC initialisation mode */
    /* save current rtc time locally */
    platform_get_rtc_time( &saved_rtc_time );
//#endif /* #ifdef RTC_ENABLED */

    /* Reset RTC values */
    reset_rtc_values();

    /* Change the clocking state of the RTC, so it ticks every 1.25ms while cpu is sleeping - 800Hz clock */
    stm32f2_rtc_change_clock( &current_clock_state, CLOCKING_EVERY_1p25MSEC );

    return WICED_SUCCESS;
}


wiced_result_t rtc_sleep_abort( void )
{
    /* Change the clocking state of the RTC, so its tick is back to normal */
    stm32f2_rtc_change_clock( &current_clock_state, CLOCKING_EVERY_SEC );

//#ifdef RTC_ENABLED /* !!If we dont set the time after the clocks have been changed. */
    /*get an error while trying to enter STM RTC initialisation mode */
    /* restore time saved before */
    platform_set_rtc_time( &saved_rtc_time );
//#endif /* #ifdef RTC_ENABLED */
    return WICED_SUCCESS;
}


wiced_result_t rtc_sleep_exit( unsigned long requested_sleep_time, unsigned long *cpu_sleep_time )
{

    uint32_t    time_units_passed_since_powersave_enter; /* time unit is 1.25ms when we are sleeping */
#ifdef WICED_ENABLE_MCU_RTC
    uint32_t    seconds_contribution;
    static int  leftover=0;
#endif /* #ifdef WICED_ENABLE_MCU_RTC */
    float       temp;

    /* Get current calendar register values and convert them to a number of 1.25ms that passed since power-save entry */
    time_units_passed_since_powersave_enter = convert_rtc_calendar_values_to_units_passed();
#ifdef WICED_ENABLE_MCU_RTC

    /* Make contribution of time-units to the current time, get the leftover that is less then a second */
    leftover+= add_1p25ms_contribution( time_units_passed_since_powersave_enter, &seconds_contribution );

    /* Subtract 1 second for every 1.25 * 800, because in reality we are running 799,2195Hz which wil give the tick value = 1.25122 */
    if( leftover > NUM_1P25MS_IN_SEC )
    {
        compensate_time_error(1, WICED_FALSE);
        leftover -= NUM_1P25MS_IN_SEC;
    }
#endif /* #ifdef WICED_ENABLE_MCU_RTC */

//#ifdef RTC_ENABLED /* !!If we dont set the time after the clocks have been changed. */
    /*get an error while trying to enter STM RTC initialisation mode */

    /* update RTC time */
    platform_set_rtc_time(&saved_rtc_time);
//#endif /* #ifdef RTC_ENABLED */

    /* Change, rtc clock state and update rtc peripheral, even when RTC is not enabled */
    /* reducing clock frequency is better for power consumption */
    stm32f2_rtc_change_clock( &current_clock_state, CLOCKING_EVERY_SEC );
    temp = (float)time_units_passed_since_powersave_enter * (float)1.25;

    /* Round up to milliseconds, not a problem if the system will get less ticks as expected, it is not a time which is populated to a user */
    if( requested_sleep_time > 1 )
    {
        if (rtc_reset_fail == 0)
            *cpu_sleep_time = (uint32_t)temp;
        else
            *cpu_sleep_time = (uint32_t)1;
    }
    else
    {
        /* When a delay was set to 1, we will get a wake up interrupt before RTC tick */
        /* we will think that there were no rtc tick, and the cpu_sleep_time will be set to 0 */
        /* operating system will miss a tick which in reality did happen */
        *cpu_sleep_time = 1;
    }

    return WICED_SUCCESS;
}

#endif /* #ifndef WICED_DISABLE_MCU_POWERSAVE */


wiced_result_t platform_set_rtc_time( wiced_rtc_time_t* time )
{
    RTC_TimeTypeDef rtc_write_time;
    RTC_DateTypeDef rtc_write_date;
    wiced_bool_t    valid = WICED_FALSE;

    WICED_VERIFY_TIME(time, valid);
    if( valid == WICED_FALSE )
    {
        return WICED_BADARG;
    }
    rtc_write_time.RTC_Seconds = time->sec;
    rtc_write_time.RTC_Minutes = time->min;
    rtc_write_time.RTC_Hours   = time->hr;
    rtc_write_date.RTC_WeekDay = time->weekday;
    rtc_write_date.RTC_Date    = time->date;
    rtc_write_date.RTC_Month   = time->month;
    rtc_write_date.RTC_Year    = time->year;


    RTC_SetTime( RTC_Format_BIN, &rtc_write_time );
    RTC_SetDate( RTC_Format_BIN, &rtc_write_date );
    return WICED_SUCCESS;
}


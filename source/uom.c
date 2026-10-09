#include <stdio.h>
#include <string.h>
#include <strings.h> // Required for strcasecmp on Linux

// Ensure your header file contains the typedef enum
#include "uom.h" 

// Structure for mapping raw string tokens to the internal Enum
typedef struct {
    const char *alias;
    acd_uom_t uom;
} uom_alias_map_t;

// Robust lookup table matching multiple string formats to single enums
static const uom_alias_map_t uom_parse_table[] = {
    // Celsius Aliases
    { "C",           UOM_CELSIUS },
    { "degC",        UOM_CELSIUS },
    { "°C",          UOM_CELSIUS },
    { "celsius",     UOM_CELSIUS },

    // Fahrenheit Aliases
    { "F",           UOM_FAHRENHEIT },
    { "degF",        UOM_FAHRENHEIT },
    { "°F",          UOM_FAHRENHEIT },
    { "fahrenheit",  UOM_FAHRENHEIT },

    // pH Aliases
    { "pH",          UOM_PH },

    // Millivolts / ORP Aliases
    { "mV",          UOM_MV },
    { "orp",         UOM_MV },

    // Millilitre 
    { "ml",          UOM_MILLILITERS },
    { "milliliters", UOM_MILLILITERS },

    { "liter",       UOM_LITERS },
    { "liters",      UOM_LITERS },
    { "l",           UOM_LITERS },
    { "gallon",      UOM_GALLONS },
    { "gallons",     UOM_GALLONS },
    { "gal",         UOM_GALLONS },


    // Pressure Aliases
    { "psi",         UOM_PSI },

    // Percentage Aliases
    { "%",           UOM_PERCENT },
    { "pct",         UOM_PERCENT },
    { "percent",     UOM_PERCENT },

    // RPM Aliases
    { "rpm",         UOM_RPM },

    // Conductivity
    { "µS/cm",       UOM_MICROSIEMENS_CM},
    { "cm",          UOM_MICROSIEMENS_CM},
    { "ppm",         UOM_PPM },
    { "PSU",         UOM_PSU },    // Practical Salinity Unit
    
    // System Metrics
    { "bytes",       UOM_BYTES },
    { "seconds",     UOM_SECONDS },
    { "sec",         UOM_SECONDS },
    { "s",           UOM_SECONDS },
    { "minutes",     UOM_MINUTES },
    { "min",         UOM_MINUTES },
    { "m",           UOM_MINUTES },
    { "ratio",       UOM_RATIO },
    { "none",        UOM_NONE }
};

#define PARSE_TABLE_SIZE (sizeof(uom_parse_table) / sizeof(uom_parse_table[0]))

/**
 * Parses an incoming string (from JSON config or Web UI) into an Enum token.
 * Falls back to UOM_CUSTOM if the unit isn't recognized but isn't empty.
 */
acd_uom_t parse_uom(const char *str) {
    if (str == NULL || *str == '\0') {
        return UOM_NONE;
    }

    // Scan through our map table using case-insensitive validation
    for (size_t i = 0; i < PARSE_TABLE_SIZE; i++) {
        if (strcasecmp(str, uom_parse_table[i].alias) == 0) {
            return uom_parse_table[i].uom;
        }
    }

    // If a string was provided but didn't match any pre-defined metric,
    // tag it as a custom dynamic unit.
    //return UOM_CUSTOM;

    return UOM_NONE;
}

/**
 * Converts an internal Enum token to a standardized clean display string.
 */
const char* uom_to_str(acd_uom_t uom) {
    switch (uom) {
        case UOM_CELSIUS:    return "°C";
        case UOM_FAHRENHEIT: return "°F";
        case UOM_PH:         return "pH";
        case UOM_MV:         return "mV";
        case UOM_PSI:        return "psi";
        case UOM_PERCENT:    return "%";
        case UOM_RPM:        return "RPM";
        case UOM_BYTES:      return "bytes";
        case UOM_SECONDS:    return "s";
        case UOM_MINUTES:    return "min";
        case UOM_RATIO:      return "ratio";
        case UOM_MILLILITERS:return "ml";
        case UOM_LITERS:     return "L";
        case UOM_GALLONS:    return "gal";
        case UOM_MICROSIEMENS_CM:  return "µS/cm"; // This is correct but too long.
        //case UOM_MICROSIEMENS_CM:  return "µS";
        case UOM_PPM:        return "ppm";
        case UOM_PSU:        return "PSU";
        case UOM_SPECIFIC_GRAVITY:return "SG";
        //case UOM_CUSTOM:     return "custom";
        case UOM_NONE:       
        default:             return "";
    }
}

const char* uom_to_fullstr(acd_uom_t uom) {
    switch (uom) {
        case UOM_CELSIUS:    return "celsius";
        case UOM_FAHRENHEIT: return "fahrenheit";
        case UOM_PH:         return "pH";
        case UOM_MV:         return "mV";
        case UOM_PSI:        return "psi";
        case UOM_PERCENT:    return "percent";
        case UOM_RPM:        return "RPM";
        case UOM_BYTES:      return "bytes";
        case UOM_SECONDS:    return "seconds";
        case UOM_MINUTES:    return "minutes";
        case UOM_RATIO:      return "ratio";
        case UOM_MILLILITERS:return "milliliters";
        case UOM_LITERS:     return "liters";
        case UOM_GALLONS:    return "gallons";
        case UOM_MICROSIEMENS_CM:  return "microsiemens per centimeter";
        case UOM_PPM:        return "parts per million";
        case UOM_PSU:        return "practical salinity units";
        case UOM_SPECIFIC_GRAVITY:return "specific gravity";
        default:             return "";
    }
}

const char* uom_to_display_str(acd_uom_t uom) {
  if (uom == UOM_PH) {
    return UOM_NONE;
  }
  
  return uom_to_str(uom);
}
#ifndef UOM_H
#define UOM_H


#define ML_PER_LITER  1000.0f
#define ML_PER_GALLON 3785.411784f // US liquid gallon


typedef enum {
    UOM_NONE = 0,
//    UOM_CUSTOM,
    UOM_CELSIUS,
    UOM_FAHRENHEIT,
    UOM_PH,
    UOM_MV,
    UOM_PSI,
    UOM_PERCENT,
    UOM_RPM,
    UOM_BYTES,
    UOM_SECONDS,
    UOM_MINUTES,
    UOM_RATIO,
    UOM_MILLILITERS,
    UOM_LITERS,
    UOM_GALLONS,
    UOM_MICROSIEMENS_CM,
    UOM_PPM,
    UOM_PSU,
    UOM_SPECIFIC_GRAVITY,
} acd_uom_t;

acd_uom_t   parse_uom(const char *str);
const char* uom_to_str(acd_uom_t uom);
const char* uom_to_fullstr(acd_uom_t uom);
const char* uom_to_display_str(acd_uom_t uom); // For display purposes (ie ph is none)

#define IS_UOM_TEMPERATURE(uom) ((uom) == UOM_CELSIUS || (uom) == UOM_FAHRENHEIT)

#endif // UOM_H
/*
 * =================================================================================
 * Project:   Lobster Lock - Self-Bondage Session Manager
 * File:      Types.h
 * =================================================================================
 */

 #include "Types.h"

const char *stateToString(DeviceState s) {
  switch (s) {
  case READY:
    return "READY";
  case ARMED:
    return "ARMED";
  case LOCKED:
    return "LOCKED";
  case ABORTED:
    return "ABORTED";
  case COMPLETED:
    return "COMPLETED";
  case TESTING:
    return "TESTING";
  default:
    return "READY";
  }
}

const char *durTypeToString(DurationType d) {
  switch (d) {
  case DUR_RANDOM:
    return "DUR_RANDOM";
  case DUR_RANGE_SHORT:
    return "DUR_RANGE_SHORT";
  case DUR_RANGE_MEDIUM:
    return "DUR_RANGE_MEDIUM";
  case DUR_RANGE_LONG:
    return "DUR_RANGE_LONG";
  default:
    return "DUR_FIXED";
  }
}

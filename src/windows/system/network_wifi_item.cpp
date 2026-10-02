#include "network_wifi_item.hpp"

// Everything is inline in the header; this translation unit exists only so AUTOMOC's
// basename-pairing (network_wifi_item.cpp <-> network_wifi_item.hpp) picks up NetworkWifiNetwork
// for mocing, matching every other Q_OBJECT header in this module (see e.g. night_light.cpp,
// services/upower/device.cpp).

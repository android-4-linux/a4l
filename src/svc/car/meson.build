# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §A01.2 (svc/car vehicle services). No GPL-3.0 sources.
#
# svc/car/ — vehicle property hub: vendored android.car (car-lib) with
# CarPropertyManager backed by a typed C++ property table (seqlock snapshot
# publication, SPSC subscription rings). Southbound adapters (mock/can/someip)
# as plugins. Automotive profile only; exported to the parent svc library.
svc_car_sources = files('car.cpp')

#pragma once
#include <d3d11.h>
int frame_worker_fixture(unsigned long long mapping, unsigned long long wake);
void check_process_handoff(ID3D11Device* device, ID3D11DeviceContext* context);

#pragma once

#if defined(gta6rt_root_signature_EXPORTS)
#define GTA6RT_ROOTSIG_API __declspec(dllexport)
#elif defined(GTA6RT_ROOT_SIGNATURE_IMPORT)
#define GTA6RT_ROOTSIG_API __declspec(dllimport)
#else
#define GTA6RT_ROOTSIG_API
#endif

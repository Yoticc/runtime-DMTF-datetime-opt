// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef CDAC_H
#define CDAC_H

class CDAC final
{
public: // static
    static CDAC Create(uint64_t descriptorAddr, ICorDebugMutableDataTarget *pDataTarget, IUnknown* legacyImpl);

public:
    CDAC() = default;

    CDAC(const CDAC&) = delete;
    CDAC& operator=(const CDAC&) = delete;

    CDAC(CDAC&& other)
        : m_module{ other.m_module }
        , m_cdac_handle{ other.m_cdac_handle }
        , m_target{ other.m_target.Extract() }
        , m_legacyImpl{ other.m_legacyImpl }
    {
        other.m_module = NULL;
        other.m_cdac_handle = 0;
        other.m_target = NULL;
        other.m_legacyImpl = NULL;
    }

    CDAC& operator=(CDAC&& other)
    {
        m_module = other.m_module;
        m_cdac_handle = other.m_cdac_handle;
        m_target = other.m_target.Extract();
        m_legacyImpl = other.m_legacyImpl;

        other.m_module = NULL;
        other.m_cdac_handle = 0;
        other.m_target = NULL;
        other.m_legacyImpl = NULL;

        return *this;
    }

    ~CDAC();

    bool IsValid() const
    {
        return m_module != NULL && m_cdac_handle != 0;
    }

    void CreateSosInterface(IUnknown** sos);

private:
    CDAC(HMODULE module, intptr_t handle, ICorDebugDataTarget* target, IUnknown* legacyImpl);

private:
    HMODULE m_module;
    intptr_t m_cdac_handle;
    NonVMComHolder<ICorDebugDataTarget> m_target;

    // Assumes the legacy impl lives for the lifetime of this class - currently ClrDataAccess, which contains this class
    IUnknown* m_legacyImpl;
};

#endif // CDAC_H

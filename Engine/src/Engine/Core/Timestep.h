#pragma once

namespace Engine
{
    class Timestep
    {
    public:
        constexpr Timestep() = default;
        constexpr explicit Timestep(float seconds)
            : m_Seconds(seconds)
        {
        }

        constexpr float GetSeconds() const { return m_Seconds; }
        constexpr float GetMilliseconds() const { return m_Seconds * 1000.0f; }

        constexpr operator float() const { return m_Seconds; }

    private:
        float m_Seconds = 0.0f;
    };
}

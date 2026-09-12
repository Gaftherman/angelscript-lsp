// Large AngelScript Benchmark Script (3000 lines)
// Generated for performance and stress testing

namespace GameMath
{
    class Vector3
    {
        float x, y, z;
        Vector3() { x = 0.0f; y = 0.0f; z = 0.0f; }
        Vector3(float _x, float _y, float _z) { x = _x; y = _y; z = _z; }
        float LengthSq() const { return x * x + y * y + z * z; }
        void Add(const Vector3 &in other) { x += other.x; y += other.y; z += other.z; }
    }
}

interface IEntity
{
    void OnInit();
    void OnUpdate(float dt);
    int GetId() const;
}

class EntityBase_0 : IEntity
    {
        private int m_id_0;
        private float m_health_0;
        private string m_name_0;
        private array<int> m_items_0;
        private GameMath::Vector3 m_pos_0;

        EntityBase_0()
        {
            m_id_0 = 0;
            m_health_0 = 100.0f;
            m_name_0 = "Entity_0";
        }

        void OnInit() override
        {
            m_items_0.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_0[j] = int(j * 1);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_0.x += dt * 2.0f;
            m_pos_0.y += dt * 3.0f;
            if (m_health_0 > 0.0f)
            {
                m_health_0 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_0;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_0 * float(multiplier);
            score += m_pos_0.LengthSq();
            return score;
        }
    }

class EntityBase_1 : IEntity
    {
        private int m_id_1;
        private float m_health_1;
        private string m_name_1;
        private array<int> m_items_1;
        private GameMath::Vector3 m_pos_1;

        EntityBase_1()
        {
            m_id_1 = 1;
            m_health_1 = 100.0f;
            m_name_1 = "Entity_1";
        }

        void OnInit() override
        {
            m_items_1.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_1[j] = int(j * 2);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_1.x += dt * 2.0f;
            m_pos_1.y += dt * 3.0f;
            if (m_health_1 > 0.0f)
            {
                m_health_1 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_1;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_1 * float(multiplier);
            score += m_pos_1.LengthSq();
            return score;
        }
    }

class EntityBase_2 : IEntity
    {
        private int m_id_2;
        private float m_health_2;
        private string m_name_2;
        private array<int> m_items_2;
        private GameMath::Vector3 m_pos_2;

        EntityBase_2()
        {
            m_id_2 = 2;
            m_health_2 = 100.0f;
            m_name_2 = "Entity_2";
        }

        void OnInit() override
        {
            m_items_2.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_2[j] = int(j * 3);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_2.x += dt * 2.0f;
            m_pos_2.y += dt * 3.0f;
            if (m_health_2 > 0.0f)
            {
                m_health_2 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_2;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_2 * float(multiplier);
            score += m_pos_2.LengthSq();
            return score;
        }
    }

class EntityBase_3 : IEntity
    {
        private int m_id_3;
        private float m_health_3;
        private string m_name_3;
        private array<int> m_items_3;
        private GameMath::Vector3 m_pos_3;

        EntityBase_3()
        {
            m_id_3 = 3;
            m_health_3 = 100.0f;
            m_name_3 = "Entity_3";
        }

        void OnInit() override
        {
            m_items_3.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_3[j] = int(j * 4);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_3.x += dt * 2.0f;
            m_pos_3.y += dt * 3.0f;
            if (m_health_3 > 0.0f)
            {
                m_health_3 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_3;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_3 * float(multiplier);
            score += m_pos_3.LengthSq();
            return score;
        }
    }

class EntityBase_4 : IEntity
    {
        private int m_id_4;
        private float m_health_4;
        private string m_name_4;
        private array<int> m_items_4;
        private GameMath::Vector3 m_pos_4;

        EntityBase_4()
        {
            m_id_4 = 4;
            m_health_4 = 100.0f;
            m_name_4 = "Entity_4";
        }

        void OnInit() override
        {
            m_items_4.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_4[j] = int(j * 5);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_4.x += dt * 2.0f;
            m_pos_4.y += dt * 3.0f;
            if (m_health_4 > 0.0f)
            {
                m_health_4 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_4;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_4 * float(multiplier);
            score += m_pos_4.LengthSq();
            return score;
        }
    }

class EntityBase_5 : IEntity
    {
        private int m_id_5;
        private float m_health_5;
        private string m_name_5;
        private array<int> m_items_5;
        private GameMath::Vector3 m_pos_5;

        EntityBase_5()
        {
            m_id_5 = 5;
            m_health_5 = 100.0f;
            m_name_5 = "Entity_5";
        }

        void OnInit() override
        {
            m_items_5.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_5[j] = int(j * 6);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_5.x += dt * 2.0f;
            m_pos_5.y += dt * 3.0f;
            if (m_health_5 > 0.0f)
            {
                m_health_5 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_5;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_5 * float(multiplier);
            score += m_pos_5.LengthSq();
            return score;
        }
    }

class EntityBase_6 : IEntity
    {
        private int m_id_6;
        private float m_health_6;
        private string m_name_6;
        private array<int> m_items_6;
        private GameMath::Vector3 m_pos_6;

        EntityBase_6()
        {
            m_id_6 = 6;
            m_health_6 = 100.0f;
            m_name_6 = "Entity_6";
        }

        void OnInit() override
        {
            m_items_6.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_6[j] = int(j * 7);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_6.x += dt * 2.0f;
            m_pos_6.y += dt * 3.0f;
            if (m_health_6 > 0.0f)
            {
                m_health_6 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_6;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_6 * float(multiplier);
            score += m_pos_6.LengthSq();
            return score;
        }
    }

class EntityBase_7 : IEntity
    {
        private int m_id_7;
        private float m_health_7;
        private string m_name_7;
        private array<int> m_items_7;
        private GameMath::Vector3 m_pos_7;

        EntityBase_7()
        {
            m_id_7 = 7;
            m_health_7 = 100.0f;
            m_name_7 = "Entity_7";
        }

        void OnInit() override
        {
            m_items_7.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_7[j] = int(j * 8);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_7.x += dt * 2.0f;
            m_pos_7.y += dt * 3.0f;
            if (m_health_7 > 0.0f)
            {
                m_health_7 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_7;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_7 * float(multiplier);
            score += m_pos_7.LengthSq();
            return score;
        }
    }

class EntityBase_8 : IEntity
    {
        private int m_id_8;
        private float m_health_8;
        private string m_name_8;
        private array<int> m_items_8;
        private GameMath::Vector3 m_pos_8;

        EntityBase_8()
        {
            m_id_8 = 8;
            m_health_8 = 100.0f;
            m_name_8 = "Entity_8";
        }

        void OnInit() override
        {
            m_items_8.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_8[j] = int(j * 9);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_8.x += dt * 2.0f;
            m_pos_8.y += dt * 3.0f;
            if (m_health_8 > 0.0f)
            {
                m_health_8 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_8;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_8 * float(multiplier);
            score += m_pos_8.LengthSq();
            return score;
        }
    }

class EntityBase_9 : IEntity
    {
        private int m_id_9;
        private float m_health_9;
        private string m_name_9;
        private array<int> m_items_9;
        private GameMath::Vector3 m_pos_9;

        EntityBase_9()
        {
            m_id_9 = 9;
            m_health_9 = 100.0f;
            m_name_9 = "Entity_9";
        }

        void OnInit() override
        {
            m_items_9.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_9[j] = int(j * 10);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_9.x += dt * 2.0f;
            m_pos_9.y += dt * 3.0f;
            if (m_health_9 > 0.0f)
            {
                m_health_9 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_9;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_9 * float(multiplier);
            score += m_pos_9.LengthSq();
            return score;
        }
    }

class EntityBase_10 : IEntity
    {
        private int m_id_10;
        private float m_health_10;
        private string m_name_10;
        private array<int> m_items_10;
        private GameMath::Vector3 m_pos_10;

        EntityBase_10()
        {
            m_id_10 = 10;
            m_health_10 = 100.0f;
            m_name_10 = "Entity_10";
        }

        void OnInit() override
        {
            m_items_10.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_10[j] = int(j * 11);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_10.x += dt * 2.0f;
            m_pos_10.y += dt * 3.0f;
            if (m_health_10 > 0.0f)
            {
                m_health_10 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_10;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_10 * float(multiplier);
            score += m_pos_10.LengthSq();
            return score;
        }
    }

class EntityBase_11 : IEntity
    {
        private int m_id_11;
        private float m_health_11;
        private string m_name_11;
        private array<int> m_items_11;
        private GameMath::Vector3 m_pos_11;

        EntityBase_11()
        {
            m_id_11 = 11;
            m_health_11 = 100.0f;
            m_name_11 = "Entity_11";
        }

        void OnInit() override
        {
            m_items_11.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_11[j] = int(j * 12);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_11.x += dt * 2.0f;
            m_pos_11.y += dt * 3.0f;
            if (m_health_11 > 0.0f)
            {
                m_health_11 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_11;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_11 * float(multiplier);
            score += m_pos_11.LengthSq();
            return score;
        }
    }

class EntityBase_12 : IEntity
    {
        private int m_id_12;
        private float m_health_12;
        private string m_name_12;
        private array<int> m_items_12;
        private GameMath::Vector3 m_pos_12;

        EntityBase_12()
        {
            m_id_12 = 12;
            m_health_12 = 100.0f;
            m_name_12 = "Entity_12";
        }

        void OnInit() override
        {
            m_items_12.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_12[j] = int(j * 13);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_12.x += dt * 2.0f;
            m_pos_12.y += dt * 3.0f;
            if (m_health_12 > 0.0f)
            {
                m_health_12 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_12;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_12 * float(multiplier);
            score += m_pos_12.LengthSq();
            return score;
        }
    }

class EntityBase_13 : IEntity
    {
        private int m_id_13;
        private float m_health_13;
        private string m_name_13;
        private array<int> m_items_13;
        private GameMath::Vector3 m_pos_13;

        EntityBase_13()
        {
            m_id_13 = 13;
            m_health_13 = 100.0f;
            m_name_13 = "Entity_13";
        }

        void OnInit() override
        {
            m_items_13.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_13[j] = int(j * 14);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_13.x += dt * 2.0f;
            m_pos_13.y += dt * 3.0f;
            if (m_health_13 > 0.0f)
            {
                m_health_13 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_13;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_13 * float(multiplier);
            score += m_pos_13.LengthSq();
            return score;
        }
    }

class EntityBase_14 : IEntity
    {
        private int m_id_14;
        private float m_health_14;
        private string m_name_14;
        private array<int> m_items_14;
        private GameMath::Vector3 m_pos_14;

        EntityBase_14()
        {
            m_id_14 = 14;
            m_health_14 = 100.0f;
            m_name_14 = "Entity_14";
        }

        void OnInit() override
        {
            m_items_14.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_14[j] = int(j * 15);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_14.x += dt * 2.0f;
            m_pos_14.y += dt * 3.0f;
            if (m_health_14 > 0.0f)
            {
                m_health_14 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_14;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_14 * float(multiplier);
            score += m_pos_14.LengthSq();
            return score;
        }
    }

class EntityBase_15 : IEntity
    {
        private int m_id_15;
        private float m_health_15;
        private string m_name_15;
        private array<int> m_items_15;
        private GameMath::Vector3 m_pos_15;

        EntityBase_15()
        {
            m_id_15 = 15;
            m_health_15 = 100.0f;
            m_name_15 = "Entity_15";
        }

        void OnInit() override
        {
            m_items_15.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_15[j] = int(j * 16);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_15.x += dt * 2.0f;
            m_pos_15.y += dt * 3.0f;
            if (m_health_15 > 0.0f)
            {
                m_health_15 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_15;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_15 * float(multiplier);
            score += m_pos_15.LengthSq();
            return score;
        }
    }

class EntityBase_16 : IEntity
    {
        private int m_id_16;
        private float m_health_16;
        private string m_name_16;
        private array<int> m_items_16;
        private GameMath::Vector3 m_pos_16;

        EntityBase_16()
        {
            m_id_16 = 16;
            m_health_16 = 100.0f;
            m_name_16 = "Entity_16";
        }

        void OnInit() override
        {
            m_items_16.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_16[j] = int(j * 17);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_16.x += dt * 2.0f;
            m_pos_16.y += dt * 3.0f;
            if (m_health_16 > 0.0f)
            {
                m_health_16 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_16;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_16 * float(multiplier);
            score += m_pos_16.LengthSq();
            return score;
        }
    }

class EntityBase_17 : IEntity
    {
        private int m_id_17;
        private float m_health_17;
        private string m_name_17;
        private array<int> m_items_17;
        private GameMath::Vector3 m_pos_17;

        EntityBase_17()
        {
            m_id_17 = 17;
            m_health_17 = 100.0f;
            m_name_17 = "Entity_17";
        }

        void OnInit() override
        {
            m_items_17.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_17[j] = int(j * 18);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_17.x += dt * 2.0f;
            m_pos_17.y += dt * 3.0f;
            if (m_health_17 > 0.0f)
            {
                m_health_17 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_17;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_17 * float(multiplier);
            score += m_pos_17.LengthSq();
            return score;
        }
    }

class EntityBase_18 : IEntity
    {
        private int m_id_18;
        private float m_health_18;
        private string m_name_18;
        private array<int> m_items_18;
        private GameMath::Vector3 m_pos_18;

        EntityBase_18()
        {
            m_id_18 = 18;
            m_health_18 = 100.0f;
            m_name_18 = "Entity_18";
        }

        void OnInit() override
        {
            m_items_18.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_18[j] = int(j * 19);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_18.x += dt * 2.0f;
            m_pos_18.y += dt * 3.0f;
            if (m_health_18 > 0.0f)
            {
                m_health_18 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_18;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_18 * float(multiplier);
            score += m_pos_18.LengthSq();
            return score;
        }
    }

class EntityBase_19 : IEntity
    {
        private int m_id_19;
        private float m_health_19;
        private string m_name_19;
        private array<int> m_items_19;
        private GameMath::Vector3 m_pos_19;

        EntityBase_19()
        {
            m_id_19 = 19;
            m_health_19 = 100.0f;
            m_name_19 = "Entity_19";
        }

        void OnInit() override
        {
            m_items_19.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_19[j] = int(j * 20);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_19.x += dt * 2.0f;
            m_pos_19.y += dt * 3.0f;
            if (m_health_19 > 0.0f)
            {
                m_health_19 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_19;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_19 * float(multiplier);
            score += m_pos_19.LengthSq();
            return score;
        }
    }

class EntityBase_20 : IEntity
    {
        private int m_id_20;
        private float m_health_20;
        private string m_name_20;
        private array<int> m_items_20;
        private GameMath::Vector3 m_pos_20;

        EntityBase_20()
        {
            m_id_20 = 20;
            m_health_20 = 100.0f;
            m_name_20 = "Entity_20";
        }

        void OnInit() override
        {
            m_items_20.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_20[j] = int(j * 21);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_20.x += dt * 2.0f;
            m_pos_20.y += dt * 3.0f;
            if (m_health_20 > 0.0f)
            {
                m_health_20 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_20;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_20 * float(multiplier);
            score += m_pos_20.LengthSq();
            return score;
        }
    }

class EntityBase_21 : IEntity
    {
        private int m_id_21;
        private float m_health_21;
        private string m_name_21;
        private array<int> m_items_21;
        private GameMath::Vector3 m_pos_21;

        EntityBase_21()
        {
            m_id_21 = 21;
            m_health_21 = 100.0f;
            m_name_21 = "Entity_21";
        }

        void OnInit() override
        {
            m_items_21.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_21[j] = int(j * 22);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_21.x += dt * 2.0f;
            m_pos_21.y += dt * 3.0f;
            if (m_health_21 > 0.0f)
            {
                m_health_21 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_21;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_21 * float(multiplier);
            score += m_pos_21.LengthSq();
            return score;
        }
    }

class EntityBase_22 : IEntity
    {
        private int m_id_22;
        private float m_health_22;
        private string m_name_22;
        private array<int> m_items_22;
        private GameMath::Vector3 m_pos_22;

        EntityBase_22()
        {
            m_id_22 = 22;
            m_health_22 = 100.0f;
            m_name_22 = "Entity_22";
        }

        void OnInit() override
        {
            m_items_22.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_22[j] = int(j * 23);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_22.x += dt * 2.0f;
            m_pos_22.y += dt * 3.0f;
            if (m_health_22 > 0.0f)
            {
                m_health_22 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_22;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_22 * float(multiplier);
            score += m_pos_22.LengthSq();
            return score;
        }
    }

class EntityBase_23 : IEntity
    {
        private int m_id_23;
        private float m_health_23;
        private string m_name_23;
        private array<int> m_items_23;
        private GameMath::Vector3 m_pos_23;

        EntityBase_23()
        {
            m_id_23 = 23;
            m_health_23 = 100.0f;
            m_name_23 = "Entity_23";
        }

        void OnInit() override
        {
            m_items_23.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_23[j] = int(j * 24);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_23.x += dt * 2.0f;
            m_pos_23.y += dt * 3.0f;
            if (m_health_23 > 0.0f)
            {
                m_health_23 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_23;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_23 * float(multiplier);
            score += m_pos_23.LengthSq();
            return score;
        }
    }

class EntityBase_24 : IEntity
    {
        private int m_id_24;
        private float m_health_24;
        private string m_name_24;
        private array<int> m_items_24;
        private GameMath::Vector3 m_pos_24;

        EntityBase_24()
        {
            m_id_24 = 24;
            m_health_24 = 100.0f;
            m_name_24 = "Entity_24";
        }

        void OnInit() override
        {
            m_items_24.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_24[j] = int(j * 25);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_24.x += dt * 2.0f;
            m_pos_24.y += dt * 3.0f;
            if (m_health_24 > 0.0f)
            {
                m_health_24 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_24;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_24 * float(multiplier);
            score += m_pos_24.LengthSq();
            return score;
        }
    }

class EntityBase_25 : IEntity
    {
        private int m_id_25;
        private float m_health_25;
        private string m_name_25;
        private array<int> m_items_25;
        private GameMath::Vector3 m_pos_25;

        EntityBase_25()
        {
            m_id_25 = 25;
            m_health_25 = 100.0f;
            m_name_25 = "Entity_25";
        }

        void OnInit() override
        {
            m_items_25.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_25[j] = int(j * 26);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_25.x += dt * 2.0f;
            m_pos_25.y += dt * 3.0f;
            if (m_health_25 > 0.0f)
            {
                m_health_25 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_25;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_25 * float(multiplier);
            score += m_pos_25.LengthSq();
            return score;
        }
    }

class EntityBase_26 : IEntity
    {
        private int m_id_26;
        private float m_health_26;
        private string m_name_26;
        private array<int> m_items_26;
        private GameMath::Vector3 m_pos_26;

        EntityBase_26()
        {
            m_id_26 = 26;
            m_health_26 = 100.0f;
            m_name_26 = "Entity_26";
        }

        void OnInit() override
        {
            m_items_26.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_26[j] = int(j * 27);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_26.x += dt * 2.0f;
            m_pos_26.y += dt * 3.0f;
            if (m_health_26 > 0.0f)
            {
                m_health_26 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_26;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_26 * float(multiplier);
            score += m_pos_26.LengthSq();
            return score;
        }
    }

class EntityBase_27 : IEntity
    {
        private int m_id_27;
        private float m_health_27;
        private string m_name_27;
        private array<int> m_items_27;
        private GameMath::Vector3 m_pos_27;

        EntityBase_27()
        {
            m_id_27 = 27;
            m_health_27 = 100.0f;
            m_name_27 = "Entity_27";
        }

        void OnInit() override
        {
            m_items_27.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_27[j] = int(j * 28);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_27.x += dt * 2.0f;
            m_pos_27.y += dt * 3.0f;
            if (m_health_27 > 0.0f)
            {
                m_health_27 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_27;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_27 * float(multiplier);
            score += m_pos_27.LengthSq();
            return score;
        }
    }

class EntityBase_28 : IEntity
    {
        private int m_id_28;
        private float m_health_28;
        private string m_name_28;
        private array<int> m_items_28;
        private GameMath::Vector3 m_pos_28;

        EntityBase_28()
        {
            m_id_28 = 28;
            m_health_28 = 100.0f;
            m_name_28 = "Entity_28";
        }

        void OnInit() override
        {
            m_items_28.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_28[j] = int(j * 29);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_28.x += dt * 2.0f;
            m_pos_28.y += dt * 3.0f;
            if (m_health_28 > 0.0f)
            {
                m_health_28 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_28;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_28 * float(multiplier);
            score += m_pos_28.LengthSq();
            return score;
        }
    }

class EntityBase_29 : IEntity
    {
        private int m_id_29;
        private float m_health_29;
        private string m_name_29;
        private array<int> m_items_29;
        private GameMath::Vector3 m_pos_29;

        EntityBase_29()
        {
            m_id_29 = 29;
            m_health_29 = 100.0f;
            m_name_29 = "Entity_29";
        }

        void OnInit() override
        {
            m_items_29.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_29[j] = int(j * 30);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_29.x += dt * 2.0f;
            m_pos_29.y += dt * 3.0f;
            if (m_health_29 > 0.0f)
            {
                m_health_29 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_29;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_29 * float(multiplier);
            score += m_pos_29.LengthSq();
            return score;
        }
    }

class EntityBase_30 : IEntity
    {
        private int m_id_30;
        private float m_health_30;
        private string m_name_30;
        private array<int> m_items_30;
        private GameMath::Vector3 m_pos_30;

        EntityBase_30()
        {
            m_id_30 = 30;
            m_health_30 = 100.0f;
            m_name_30 = "Entity_30";
        }

        void OnInit() override
        {
            m_items_30.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_30[j] = int(j * 31);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_30.x += dt * 2.0f;
            m_pos_30.y += dt * 3.0f;
            if (m_health_30 > 0.0f)
            {
                m_health_30 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_30;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_30 * float(multiplier);
            score += m_pos_30.LengthSq();
            return score;
        }
    }

class EntityBase_31 : IEntity
    {
        private int m_id_31;
        private float m_health_31;
        private string m_name_31;
        private array<int> m_items_31;
        private GameMath::Vector3 m_pos_31;

        EntityBase_31()
        {
            m_id_31 = 31;
            m_health_31 = 100.0f;
            m_name_31 = "Entity_31";
        }

        void OnInit() override
        {
            m_items_31.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_31[j] = int(j * 32);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_31.x += dt * 2.0f;
            m_pos_31.y += dt * 3.0f;
            if (m_health_31 > 0.0f)
            {
                m_health_31 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_31;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_31 * float(multiplier);
            score += m_pos_31.LengthSq();
            return score;
        }
    }

class EntityBase_32 : IEntity
    {
        private int m_id_32;
        private float m_health_32;
        private string m_name_32;
        private array<int> m_items_32;
        private GameMath::Vector3 m_pos_32;

        EntityBase_32()
        {
            m_id_32 = 32;
            m_health_32 = 100.0f;
            m_name_32 = "Entity_32";
        }

        void OnInit() override
        {
            m_items_32.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_32[j] = int(j * 33);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_32.x += dt * 2.0f;
            m_pos_32.y += dt * 3.0f;
            if (m_health_32 > 0.0f)
            {
                m_health_32 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_32;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_32 * float(multiplier);
            score += m_pos_32.LengthSq();
            return score;
        }
    }

class EntityBase_33 : IEntity
    {
        private int m_id_33;
        private float m_health_33;
        private string m_name_33;
        private array<int> m_items_33;
        private GameMath::Vector3 m_pos_33;

        EntityBase_33()
        {
            m_id_33 = 33;
            m_health_33 = 100.0f;
            m_name_33 = "Entity_33";
        }

        void OnInit() override
        {
            m_items_33.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_33[j] = int(j * 34);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_33.x += dt * 2.0f;
            m_pos_33.y += dt * 3.0f;
            if (m_health_33 > 0.0f)
            {
                m_health_33 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_33;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_33 * float(multiplier);
            score += m_pos_33.LengthSq();
            return score;
        }
    }

class EntityBase_34 : IEntity
    {
        private int m_id_34;
        private float m_health_34;
        private string m_name_34;
        private array<int> m_items_34;
        private GameMath::Vector3 m_pos_34;

        EntityBase_34()
        {
            m_id_34 = 34;
            m_health_34 = 100.0f;
            m_name_34 = "Entity_34";
        }

        void OnInit() override
        {
            m_items_34.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_34[j] = int(j * 35);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_34.x += dt * 2.0f;
            m_pos_34.y += dt * 3.0f;
            if (m_health_34 > 0.0f)
            {
                m_health_34 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_34;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_34 * float(multiplier);
            score += m_pos_34.LengthSq();
            return score;
        }
    }

class EntityBase_35 : IEntity
    {
        private int m_id_35;
        private float m_health_35;
        private string m_name_35;
        private array<int> m_items_35;
        private GameMath::Vector3 m_pos_35;

        EntityBase_35()
        {
            m_id_35 = 35;
            m_health_35 = 100.0f;
            m_name_35 = "Entity_35";
        }

        void OnInit() override
        {
            m_items_35.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_35[j] = int(j * 36);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_35.x += dt * 2.0f;
            m_pos_35.y += dt * 3.0f;
            if (m_health_35 > 0.0f)
            {
                m_health_35 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_35;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_35 * float(multiplier);
            score += m_pos_35.LengthSq();
            return score;
        }
    }

class EntityBase_36 : IEntity
    {
        private int m_id_36;
        private float m_health_36;
        private string m_name_36;
        private array<int> m_items_36;
        private GameMath::Vector3 m_pos_36;

        EntityBase_36()
        {
            m_id_36 = 36;
            m_health_36 = 100.0f;
            m_name_36 = "Entity_36";
        }

        void OnInit() override
        {
            m_items_36.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_36[j] = int(j * 37);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_36.x += dt * 2.0f;
            m_pos_36.y += dt * 3.0f;
            if (m_health_36 > 0.0f)
            {
                m_health_36 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_36;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_36 * float(multiplier);
            score += m_pos_36.LengthSq();
            return score;
        }
    }

class EntityBase_37 : IEntity
    {
        private int m_id_37;
        private float m_health_37;
        private string m_name_37;
        private array<int> m_items_37;
        private GameMath::Vector3 m_pos_37;

        EntityBase_37()
        {
            m_id_37 = 37;
            m_health_37 = 100.0f;
            m_name_37 = "Entity_37";
        }

        void OnInit() override
        {
            m_items_37.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_37[j] = int(j * 38);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_37.x += dt * 2.0f;
            m_pos_37.y += dt * 3.0f;
            if (m_health_37 > 0.0f)
            {
                m_health_37 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_37;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_37 * float(multiplier);
            score += m_pos_37.LengthSq();
            return score;
        }
    }

class EntityBase_38 : IEntity
    {
        private int m_id_38;
        private float m_health_38;
        private string m_name_38;
        private array<int> m_items_38;
        private GameMath::Vector3 m_pos_38;

        EntityBase_38()
        {
            m_id_38 = 38;
            m_health_38 = 100.0f;
            m_name_38 = "Entity_38";
        }

        void OnInit() override
        {
            m_items_38.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_38[j] = int(j * 39);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_38.x += dt * 2.0f;
            m_pos_38.y += dt * 3.0f;
            if (m_health_38 > 0.0f)
            {
                m_health_38 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_38;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_38 * float(multiplier);
            score += m_pos_38.LengthSq();
            return score;
        }
    }

class EntityBase_39 : IEntity
    {
        private int m_id_39;
        private float m_health_39;
        private string m_name_39;
        private array<int> m_items_39;
        private GameMath::Vector3 m_pos_39;

        EntityBase_39()
        {
            m_id_39 = 39;
            m_health_39 = 100.0f;
            m_name_39 = "Entity_39";
        }

        void OnInit() override
        {
            m_items_39.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_39[j] = int(j * 40);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_39.x += dt * 2.0f;
            m_pos_39.y += dt * 3.0f;
            if (m_health_39 > 0.0f)
            {
                m_health_39 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_39;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_39 * float(multiplier);
            score += m_pos_39.LengthSq();
            return score;
        }
    }

class EntityBase_40 : IEntity
    {
        private int m_id_40;
        private float m_health_40;
        private string m_name_40;
        private array<int> m_items_40;
        private GameMath::Vector3 m_pos_40;

        EntityBase_40()
        {
            m_id_40 = 40;
            m_health_40 = 100.0f;
            m_name_40 = "Entity_40";
        }

        void OnInit() override
        {
            m_items_40.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_40[j] = int(j * 41);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_40.x += dt * 2.0f;
            m_pos_40.y += dt * 3.0f;
            if (m_health_40 > 0.0f)
            {
                m_health_40 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_40;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_40 * float(multiplier);
            score += m_pos_40.LengthSq();
            return score;
        }
    }

class EntityBase_41 : IEntity
    {
        private int m_id_41;
        private float m_health_41;
        private string m_name_41;
        private array<int> m_items_41;
        private GameMath::Vector3 m_pos_41;

        EntityBase_41()
        {
            m_id_41 = 41;
            m_health_41 = 100.0f;
            m_name_41 = "Entity_41";
        }

        void OnInit() override
        {
            m_items_41.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_41[j] = int(j * 42);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_41.x += dt * 2.0f;
            m_pos_41.y += dt * 3.0f;
            if (m_health_41 > 0.0f)
            {
                m_health_41 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_41;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_41 * float(multiplier);
            score += m_pos_41.LengthSq();
            return score;
        }
    }

class EntityBase_42 : IEntity
    {
        private int m_id_42;
        private float m_health_42;
        private string m_name_42;
        private array<int> m_items_42;
        private GameMath::Vector3 m_pos_42;

        EntityBase_42()
        {
            m_id_42 = 42;
            m_health_42 = 100.0f;
            m_name_42 = "Entity_42";
        }

        void OnInit() override
        {
            m_items_42.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_42[j] = int(j * 43);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_42.x += dt * 2.0f;
            m_pos_42.y += dt * 3.0f;
            if (m_health_42 > 0.0f)
            {
                m_health_42 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_42;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_42 * float(multiplier);
            score += m_pos_42.LengthSq();
            return score;
        }
    }

class EntityBase_43 : IEntity
    {
        private int m_id_43;
        private float m_health_43;
        private string m_name_43;
        private array<int> m_items_43;
        private GameMath::Vector3 m_pos_43;

        EntityBase_43()
        {
            m_id_43 = 43;
            m_health_43 = 100.0f;
            m_name_43 = "Entity_43";
        }

        void OnInit() override
        {
            m_items_43.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_43[j] = int(j * 44);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_43.x += dt * 2.0f;
            m_pos_43.y += dt * 3.0f;
            if (m_health_43 > 0.0f)
            {
                m_health_43 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_43;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_43 * float(multiplier);
            score += m_pos_43.LengthSq();
            return score;
        }
    }

class EntityBase_44 : IEntity
    {
        private int m_id_44;
        private float m_health_44;
        private string m_name_44;
        private array<int> m_items_44;
        private GameMath::Vector3 m_pos_44;

        EntityBase_44()
        {
            m_id_44 = 44;
            m_health_44 = 100.0f;
            m_name_44 = "Entity_44";
        }

        void OnInit() override
        {
            m_items_44.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_44[j] = int(j * 45);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_44.x += dt * 2.0f;
            m_pos_44.y += dt * 3.0f;
            if (m_health_44 > 0.0f)
            {
                m_health_44 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_44;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_44 * float(multiplier);
            score += m_pos_44.LengthSq();
            return score;
        }
    }

class EntityBase_45 : IEntity
    {
        private int m_id_45;
        private float m_health_45;
        private string m_name_45;
        private array<int> m_items_45;
        private GameMath::Vector3 m_pos_45;

        EntityBase_45()
        {
            m_id_45 = 45;
            m_health_45 = 100.0f;
            m_name_45 = "Entity_45";
        }

        void OnInit() override
        {
            m_items_45.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_45[j] = int(j * 46);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_45.x += dt * 2.0f;
            m_pos_45.y += dt * 3.0f;
            if (m_health_45 > 0.0f)
            {
                m_health_45 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_45;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_45 * float(multiplier);
            score += m_pos_45.LengthSq();
            return score;
        }
    }

class EntityBase_46 : IEntity
    {
        private int m_id_46;
        private float m_health_46;
        private string m_name_46;
        private array<int> m_items_46;
        private GameMath::Vector3 m_pos_46;

        EntityBase_46()
        {
            m_id_46 = 46;
            m_health_46 = 100.0f;
            m_name_46 = "Entity_46";
        }

        void OnInit() override
        {
            m_items_46.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_46[j] = int(j * 47);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_46.x += dt * 2.0f;
            m_pos_46.y += dt * 3.0f;
            if (m_health_46 > 0.0f)
            {
                m_health_46 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_46;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_46 * float(multiplier);
            score += m_pos_46.LengthSq();
            return score;
        }
    }

class EntityBase_47 : IEntity
    {
        private int m_id_47;
        private float m_health_47;
        private string m_name_47;
        private array<int> m_items_47;
        private GameMath::Vector3 m_pos_47;

        EntityBase_47()
        {
            m_id_47 = 47;
            m_health_47 = 100.0f;
            m_name_47 = "Entity_47";
        }

        void OnInit() override
        {
            m_items_47.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_47[j] = int(j * 48);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_47.x += dt * 2.0f;
            m_pos_47.y += dt * 3.0f;
            if (m_health_47 > 0.0f)
            {
                m_health_47 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_47;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_47 * float(multiplier);
            score += m_pos_47.LengthSq();
            return score;
        }
    }

class EntityBase_48 : IEntity
    {
        private int m_id_48;
        private float m_health_48;
        private string m_name_48;
        private array<int> m_items_48;
        private GameMath::Vector3 m_pos_48;

        EntityBase_48()
        {
            m_id_48 = 48;
            m_health_48 = 100.0f;
            m_name_48 = "Entity_48";
        }

        void OnInit() override
        {
            m_items_48.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_48[j] = int(j * 49);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_48.x += dt * 2.0f;
            m_pos_48.y += dt * 3.0f;
            if (m_health_48 > 0.0f)
            {
                m_health_48 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_48;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_48 * float(multiplier);
            score += m_pos_48.LengthSq();
            return score;
        }
    }

class EntityBase_49 : IEntity
    {
        private int m_id_49;
        private float m_health_49;
        private string m_name_49;
        private array<int> m_items_49;
        private GameMath::Vector3 m_pos_49;

        EntityBase_49()
        {
            m_id_49 = 49;
            m_health_49 = 100.0f;
            m_name_49 = "Entity_49";
        }

        void OnInit() override
        {
            m_items_49.resize(10);
            for (uint j = 0; j < 10; ++j)
            {
                m_items_49[j] = int(j * 50);
            }
        }

        void OnUpdate(float dt) override
        {
            m_pos_49.x += dt * 2.0f;
            m_pos_49.y += dt * 3.0f;
            if (m_health_49 > 0.0f)
            {
                m_health_49 -= dt * 0.1f;
            }
        }

        int GetId() const override
        {
            return m_id_49;
        }

        float ComputeScore(int multiplier) const
        {
            float score = m_health_49 * float(multiplier);
            score += m_pos_49.LengthSq();
            return score;
        }
    }

class SimulationManager
{
    private array<IEntity@> m_entities;

    void Setup()
    {
        m_entities.insertLast(EntityBase_0());
        m_entities.insertLast(EntityBase_1());
        m_entities.insertLast(EntityBase_2());
        m_entities.insertLast(EntityBase_3());
        m_entities.insertLast(EntityBase_4());
        m_entities.insertLast(EntityBase_5());
        m_entities.insertLast(EntityBase_6());
        m_entities.insertLast(EntityBase_7());
        m_entities.insertLast(EntityBase_8());
        m_entities.insertLast(EntityBase_9());
        m_entities.insertLast(EntityBase_10());
        m_entities.insertLast(EntityBase_11());
        m_entities.insertLast(EntityBase_12());
        m_entities.insertLast(EntityBase_13());
        m_entities.insertLast(EntityBase_14());
        m_entities.insertLast(EntityBase_15());
        m_entities.insertLast(EntityBase_16());
        m_entities.insertLast(EntityBase_17());
        m_entities.insertLast(EntityBase_18());
        m_entities.insertLast(EntityBase_19());
        m_entities.insertLast(EntityBase_20());
        m_entities.insertLast(EntityBase_21());
        m_entities.insertLast(EntityBase_22());
        m_entities.insertLast(EntityBase_23());
        m_entities.insertLast(EntityBase_24());
        m_entities.insertLast(EntityBase_25());
        m_entities.insertLast(EntityBase_26());
        m_entities.insertLast(EntityBase_27());
        m_entities.insertLast(EntityBase_28());
        m_entities.insertLast(EntityBase_29());
        m_entities.insertLast(EntityBase_30());
        m_entities.insertLast(EntityBase_31());
        m_entities.insertLast(EntityBase_32());
        m_entities.insertLast(EntityBase_33());
        m_entities.insertLast(EntityBase_34());
        m_entities.insertLast(EntityBase_35());
        m_entities.insertLast(EntityBase_36());
        m_entities.insertLast(EntityBase_37());
        m_entities.insertLast(EntityBase_38());
        m_entities.insertLast(EntityBase_39());
        m_entities.insertLast(EntityBase_40());
        m_entities.insertLast(EntityBase_41());
        m_entities.insertLast(EntityBase_42());
        m_entities.insertLast(EntityBase_43());
        m_entities.insertLast(EntityBase_44());
        m_entities.insertLast(EntityBase_45());
        m_entities.insertLast(EntityBase_46());
        m_entities.insertLast(EntityBase_47());
        m_entities.insertLast(EntityBase_48());
        m_entities.insertLast(EntityBase_49());
    }

    void UpdateAll(float dt)
    {
        for (uint i = 0; i < m_entities.length(); ++i)
        {
            if (m_entities[i] !is null)
            {
                m_entities[i].OnUpdate(dt);
            }
        }
    }
}

int HelperFunction_0(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_1(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_2(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_3(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_4(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_5(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_6(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_7(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_8(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_9(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_10(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_11(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_12(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_13(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_14(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_15(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_16(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_17(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_18(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_19(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_20(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_21(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_22(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_23(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_24(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_25(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_26(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_27(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_28(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_29(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_30(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_31(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_32(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_33(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_34(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_35(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_36(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_37(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_38(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_39(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_40(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_41(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_42(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_43(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_44(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_45(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_46(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_47(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_48(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

int HelperFunction_49(int a, int b)
{
    int c = a + b;
    int d = c * 2;
    if (d > 100)
    {
        return d - 100;
    }
    return d;
}

void MainEntry()
{
    SimulationManager sim;
    sim.Setup();
    sim.UpdateAll(0.016f);
}
// End of benchmark script padding line 2999
// End of benchmark script padding line 3000

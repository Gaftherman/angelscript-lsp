interface IAnimal {
    void Speak();
    int GetAge();
}

class Dog : IAnimal {
    void Speak() {}
    int GetAge() { return 5; }
}

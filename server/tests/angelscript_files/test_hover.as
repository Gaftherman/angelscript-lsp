// ==========================================
// AngelScript LSP Hover Testing Script v2
// ==========================================
// Este archivo está diseñado para probar el nuevo y elegante formato de Hover.
// Pasa el ratón (hover) sobre cada una de las variables y funciones.

/**
 * @brief Obtiene el jugador más cercano en el mapa.
 * 
 * Esta función escanea todos los chunks activos para devolver la entidad
 * que pertenece al jugador que esté a menor distancia del origen dado.
 * 
 * @param x Posición X de búsqueda
 * @param y Posición Y de búsqueda
 * @return La instancia de Entity que representa al jugador. Devuelve nulo si no hay jugadores.
 * @throws MapNotLoadedException Si el mapa no está en memoria.
 * @note Operación costosa. Úsala preferiblemente dentro de una corrutina.
 * @warning No usar dentro del hilo de renderizado principal.
 * @deprecated Usa en su lugar GetClosestPlayerAsync.
 */
Entity GetClosestPlayer(float x, float y) {
    return Entity();
}

class Entity {
    void Destroy() {}
}

// ----------------------------------------------------
// Prueba de Sobrecargas (Overloads)
// Pasa el mouse sobre cualquiera de las llamadas a 'CalcularFisicas'
// ----------------------------------------------------
void CalcularFisicas() {}
void CalcularFisicas(float dt) {}
void CalcularFisicas(float dt, int iteraciones) {}
void CalcularFisicas(float dt, int iteraciones, bool usarGravedad) {}

// ----------------------------------------------------
// Prueba de Autocompletado y Plantillas Genéricas
// ----------------------------------------------------
class array<T> {
    void insertLast(const T&in value) {}
}

void main() {
    // 1. Hover en 'GetClosestPlayer' para ver todos los íconos hermosos (Lanza, Nota, Advertencia)
    Entity p = GetClosestPlayer(0.0f, 0.0f);

    // 2. Hover en 'CalcularFisicas' para ver "*+3 sobrecargas disponibles*"
    CalcularFisicas(0.16f);

    // 3. Hover en 'auto' para probar la inferencia AST avanzada
    auto velocidad = 120.5f;
    auto nombre = "Heroe";
    auto estaVivo = true;

    // 4. Hover en 'insertLast' para ver la sustitución de <T> por <float>
    array<float> listaTiempos;
    listaTiempos.insertLast(velocidad);
}

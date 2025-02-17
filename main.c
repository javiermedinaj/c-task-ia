#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "config.h" 

#define MAX_TAREAS 100
#define MAX_LONGITUD 256

struct string
{
    char *ptr;
    size_t len;
};

size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s)
{
    size_t new_len = s->len + size * nmemb;
    s->ptr = realloc(s->ptr, new_len + 1);
    memcpy(s->ptr + s->len, ptr, size * nmemb);
    s->ptr[new_len] = '\0';
    s->len = new_len;
    return size * nmemb;
}

typedef struct
{
    int id;
    char descripcion[MAX_LONGITUD];
} Tarea;

Tarea tareas[MAX_TAREAS];
int numTareas = 0;

void listarTareas()
{
    sqlite3 *db;
    sqlite3_stmt *res;

    int rc = sqlite3_open("tareas.db", &db);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "No se puede abrir la base de datos: %s\n", sqlite3_errmsg(db));
        return;
    }

    const char *sql = "SELECT * FROM tareas";
    rc = sqlite3_prepare_v2(db, sql, -1, &res, 0);

    printf("Tareas:\n");
    while (sqlite3_step(res) == SQLITE_ROW)
    {
        printf("%d. %s\n", sqlite3_column_int(res, 0), sqlite3_column_text(res, 1));
    }

    sqlite3_finalize(res);
    sqlite3_close(db);
}


void ayudaIA()
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    char prompt[4096] = "Ayúdame con esta tarea:\n";
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    struct string s = {0};
    int taskId;

    listarTareas();
    printf("\nIngrese el ID de la tarea para la que necesita ayuda: ");
    scanf("%d", &taskId);
    getchar(); 

    int rc = sqlite3_open("tareas.db", &db);
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT descripcion FROM tareas WHERE id=%d", taskId);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        snprintf(prompt + strlen(prompt), sizeof(prompt) - strlen(prompt),
                 "- %s\n", sqlite3_column_text(stmt, 0));
    }
    else
    {
        printf("No se encontró la tarea con ID %d\n", taskId);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    curl = curl_easy_init();
    s.len = 0;
    s.ptr = malloc(1);
    s.ptr[0] = '\0';

    struct json_object *json = json_object_new_object();
    struct json_object *messages = json_object_new_array();
    struct json_object *message = json_object_new_object();

    json_object_object_add(message, "role", json_object_new_string("user"));
    json_object_object_add(message, "content", json_object_new_string(prompt));
    json_object_array_add(messages, message);

    json_object_object_add(json, "messages", messages);
    json_object_object_add(json, "max_tokens", json_object_new_int(800));
    json_object_object_add(json, "temperature", json_object_new_double(0.7));
    json_object_object_add(json, "stop", json_object_new_null());
    json_object_object_add(json, "top_p", json_object_new_double(0.95));

    const char *json_string = json_object_to_json_string(json);

    if (!curl)
    {
        fprintf(stderr, "Error initializing CURL\n");
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, OPENAI_API_URL); 
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "api-key: " OPENAI_API_KEY); 
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_string);

    CURLcode curl_result = curl_easy_perform(curl);

    if (curl_result != CURLE_OK)
    {
        fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(curl_result));
        goto cleanup;
    }

    struct json_object *parsed_json = json_tokener_parse(s.ptr);
    if (!parsed_json)
    {
        fprintf(stderr, "Error parsing JSON response\n");
        goto cleanup;
    }

    struct json_object *choices;
    if (!json_object_object_get_ex(parsed_json, "choices", &choices) ||
        !json_object_is_type(choices, json_type_array) ||
        json_object_array_length(choices) == 0)
    {
        fprintf(stderr, "Invalid JSON structure: missing or empty choices array\n");
        json_object_put(parsed_json);
        goto cleanup;
    }

    struct json_object *first_choice = json_object_array_get_idx(choices, 0);
    struct json_object *message_obj, *content;

    if (!json_object_object_get_ex(first_choice, "message", &message_obj) ||
        !json_object_object_get_ex(message_obj, "content", &content))
    {
        fprintf(stderr, "Invalid JSON structure: missing message or content\n");
        json_object_put(parsed_json);
        goto cleanup;
    }

    printf("\nRespuesta de la IA:\n%s\n", json_object_get_string(content));
    json_object_put(parsed_json);

cleanup:
    if (curl)
        curl_easy_cleanup(curl);
    if (headers)
        curl_slist_free_all(headers);
    if (s.ptr)
        free(s.ptr);
    json_object_put(json);
}

void inicializarDB()
{
    sqlite3 *db;
    char *err_msg = 0;

    int rc = sqlite3_open("tareas.db", &db);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "No se puede abrir la base de datos: %s\n", sqlite3_errmsg(db));
        return;
    }

    const char *sql = "CREATE TABLE IF NOT EXISTS tareas (id INTEGER PRIMARY KEY AUTOINCREMENT, descripcion TEXT);";
    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);

    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "Error en SQL: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    else
    {
        fprintf(stdout, "Tabla creada correctamente\n");
    }

    sqlite3_close(db);
}

void agregarTarea(char *descripcion)
{
    sqlite3 *db;
    char *err_msg = 0;

    int rc = sqlite3_open("tareas.db", &db);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "No se puede abrir la base de datos: %s\n", sqlite3_errmsg(db));
        return;
    }

    char sql[MAX_LONGITUD];
    snprintf(sql, sizeof(sql), "INSERT INTO tareas (descripcion) VALUES ('%s');", descripcion);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "Error en SQL: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    else
    {
        fprintf(stdout, "Tarea agregada correctamente\n");
    }

    sqlite3_close(db);
}


void eliminarTarea(int id)
{
    if (id > 0 && id <= numTareas)
    {
        for (int i = id - 1; i < numTareas - 1; i++)
        {
            tareas[i] = tareas[i + 1];
        }
        numTareas--;
        printf("Tarea eliminada.\n");
    }
    else
    {
        printf("ID no válido.\n");
    }
}

int main()
{
    inicializarDB();
    int opcion;
    char descripcion[MAX_LONGITUD];
    int id;

    while (1)
    {
        printf("\n1. Agregar tarea\n2. Listar tareas\n3. Eliminar tarea\n4. Ayudar con IA\n5. Salir\nSeleccione una opción: ");
        scanf("%d", &opcion);
        getchar();

        switch (opcion)
        {
        case 1:
            printf("Ingrese la descripción de la tarea: ");
            fgets(descripcion, MAX_LONGITUD, stdin);
            descripcion[strcspn(descripcion, "\n")] = 0;
            agregarTarea(descripcion);
            break;
        case 2:
            listarTareas();
            break;
        case 3:
            printf("Ingrese el ID de la tarea a eliminar: ");
            scanf("%d", &id);
            eliminarTarea(id);
            break;
        case 4:
            listarTareas();
            printf("\nIngrese el ID de la tarea para la que necesita ayuda: ");
            scanf("%d", &id);
            getchar(); 
            ayudaIA(id);
            break;
        case 5:
            printf("Saliendo...\n");
            return 0;
        default:
            printf("Opción no válida.\n");
        }
    }
}

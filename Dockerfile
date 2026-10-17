FROM gcc:14

WORKDIR /app

# Copiar archivos del proyecto
COPY . /app

# Limpiar y compilar el proyecto con make
RUN make clean && make

# Comando por defecto al ejecutar el contenedor
CMD ["./colector", "3", "5"]

FROM gcc:14

WORKDIR /app

# Copiar archivos del proyecto
COPY . /app

# Limpiar y compilar el proyecto con make
RUN make clean && make

# Comando por defecto al ejecutar el contenedor:
# 3 sensores, 500 mediciones c/u, 4 workers, reporte SIGALRM cada 2 s, umbral 90
CMD ["./colector", "3", "500", "4", "2", "90"]

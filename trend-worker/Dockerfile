FROM golang:1.24 AS build

WORKDIR /app
COPY go.mod ./
COPY go.sum ./
COPY vendor ./vendor
COPY main.go ./
ENV GOFLAGS=-mod=vendor
ENV CGO_ENABLED=0 
RUN GOOS=linux GOARCH=arm64 go build -o /app-bin main.go

FROM debian:bookworm-slim
#RUN apt-get update && apt-get install -y ca-certificates tzdata && rm -rf /var/lib/apt/lists/*
COPY --from=build /app-bin /app-bin
ENTRYPOINT ["/app-bin"]

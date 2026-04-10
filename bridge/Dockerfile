FROM golang:1.24 AS build

WORKDIR /app
COPY go.mod ./
COPY go.sum ./
COPY vendor ./vendor
COPY main.go ./
ENV GOFLAGS=-mod=vendor
ENV CGO_ENABLED=0 
RUN GOOS=linux GOARCH=arm64 go build -o /bridge main.go

FROM gcr.io/distroless/base-debian12
COPY --from=build /bridge /bridge
ENTRYPOINT ["/bridge"]

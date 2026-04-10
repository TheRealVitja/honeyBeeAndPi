FROM golang:1.24 AS build

WORKDIR /app
COPY go.mod ./

COPY main.go ./
ENV GOFLAGS=-mod=vendor
RUN CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o /gts-worker main.go

FROM gcr.io/distroless/base-debian12
COPY --from=build /gts-worker /gts-worker
ENTRYPOINT ["/gts-worker"]

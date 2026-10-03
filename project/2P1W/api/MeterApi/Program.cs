using MeterApi;

var builder = WebApplication.CreateBuilder(args);
builder.Services.Configure<MqttOptions>(builder.Configuration.GetSection("Mqtt"));
builder.Services.AddSingleton<LatestReadingStore>();
builder.Services.AddHostedService<MqttTelemetryService>();
builder.Services.AddControllers();
builder.Services.AddEndpointsApiExplorer();
builder.Services.AddSwaggerGen();

var app = builder.Build();

if (app.Environment.IsDevelopment())
{
	app.UseSwagger();
	app.UseSwaggerUI();
}

app.MapGet("/", () => Results.Ok(new { service = "MFM Meter API", readings = "/api/readings" }));
app.MapGet("/api/readings", (LatestReadingStore store) =>
{
	var reading = store.GetLatest();
	return Results.Ok(new Dictionary<string, object?>
	{
		["available"] = reading is not null,
		["LNVolt"] = reading?.L1Voltage,
		["LNA"] = reading?.L1Current,
		["receivedAtUtc"] = reading?.ReceivedAtUtc
	});
});
app.MapControllers();

app.Run();

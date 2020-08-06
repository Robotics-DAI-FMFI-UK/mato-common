// utility functions
void mato_pack_packet(uint8_t *data_packet, uint8_t *data_to_pack, int data_size)
{
	if(data_size <= 127)
	{
		data_packet=malloc(data_size+1)
		data_packet[0] = data_size;
		memcpy(data_packet + 1, data_to_pack, data_size);
	}
	else
		if(data_size<=16383)
		{
			data_packet=malloc(data_size+2)
			data_packet[0]=data_size+128;
			data_packet[1]=data_size>>7;
			memcpy(data_packet + 2, data_to_pack, data_size);
		}
		else
			if(data_size<=2097151)
			{
				data_packet=malloc(data_size+3)
				data_packet[0]=data_size+128;
				data_packet[1]=data_size>>7;
				data_packet[2]=data_size>>7;
				memcpy(data_packet + 3, data_to_pack, data_size);
			}
}

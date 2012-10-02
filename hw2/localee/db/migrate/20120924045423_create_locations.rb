class CreateLocations < ActiveRecord::Migration
  def change
    create_table :locations do |t|
      t.string :name
      t.double :latitude
      t.double :longitude

      t.timestamps
    end
  end
end
